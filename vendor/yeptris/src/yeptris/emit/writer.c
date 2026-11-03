/* writer.c — the single writer engine (TODO.impl/13).
 *
 * One recursive emitter drives both sizing (dry: counts only) and
 * output (wet): the size query and the bytes agree by construction.
 *
 * Layout contract (indent = the CONTENT column of the node):
 *   block map value:  "key: <inline>" or "key:\n<indent+2>…"
 *   block seq item:   "- <inline>" (compact: nested block collections
 *                     start on the dash line at indent+2)
 *   flow collections re-emit compact: {k: v, k2: v2} / [a, b]
 *   scalars re-emit their recorded style; plain values that are not
 *   plain-safe in position fall back to double quotes; any multiline
 *   scalar emits as a literal block (deterministic upgrade: parse of
 *   the upgrade records literal, so byte stability holds)
 *   anchors: "&name " before the node; aliases: "*name"
 *   multi-document streams mark every document with "---"
 */

#include "writer.h"

#include "../scan/scan.h"

#include <string.h>

#include "style.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../include/yeptris/resolve.h"
#include "float/api.h"
#include "resolve/resolver.h"

/* Decodes a compact node string for the writer's configured run. */
static yep_view wv(const yep_writer* w, yep_sview sv) {
    yep_view v = {NULL, 0};
    if (sv.len == 0) {
        return v;
    }
    v.len = sv.len;
    v.p = (sv.off & YEP_SV_INPUT ? w->sv_arena : w->sv_input) + (sv.off & YEP_SV_OFF);
    return v;
}

/* Streaming flush (13C): append-only output means any high-water
 * crossing is a safe cut point. */
static void wr_maybe_flush(yep_writer* w) {
    if (w->sink == NULL || w->dry || w->len < w->watermark) {
        return;
    }
    if (w->sink(w->sink_ctx, w->p, w->len) != 0) {
        w->sink_aborted = 1;
        return;
    }
    w->flushed += w->len;
    w->len = 0;
}

static void wr_col_update(yep_writer* w, const char* p, uint32_t n) {
    for (uint32_t i = n; i > 0; i--) { /* last newline wins */
        if (p[i - 1] == '\n') {
            w->col = (int)(n - i);
            return;
        }
    }
    w->col += (int)n;
}

static void wr_reserve(yep_writer* w, size_t need);

static void wr_grow(yep_writer* w, size_t need) {
    if (w->oom || w->p == NULL) {
        w->oom = w->p == NULL;
        return;
    }
    size_t want = w->len + need + 1;
    if (want < w->cap * 2) {
        want = w->cap * 2;
    }
    char* np = (char*)realloc(w->p, want);
    if (np == NULL) {
        w->oom = 1;
        return;
    }
    w->p = np;
    w->cap = want;
}

static void wr_put(yep_writer* w, const char* p, uint32_t n) {
    if (n > 0) {
        w->last = p[n - 1];
    }
    wr_col_update(w, p, n);
    if (w->dry) {
        w->len += n;
        return;
    }
    if (w->sink != NULL && !w->grow && !w->bounded && w->cap > 0 &&
        (size_t)n > w->cap - w->len - 1) {
        /* the fixed-window streaming route cannot hold this put even
         * after the watermark flush: CHUNK it — window-sized pieces
         * with a flush between each. Append-only output makes any cut
         * a safe cut; the sink sees more, smaller chunks. Memory stays
         * bounded by the window (the route's whole point) — the old
         * unconditional memcpy wrote straight past it for any scalar
         * larger than the remaining room (the 2026-10-31 audit's
         * streaming-overflow finding). */
        size_t off = 0;
        while (off < (size_t)n) {
            if (w->sink_aborted) {
                w->len += (size_t)n - off; /* keep counting: the caller fails */
                return;
            }
            size_t room = w->cap - w->len - 1;
            if (room > (size_t)n - off) {
                room = (size_t)n - off;
            }
            memcpy(w->p + w->len, p + off, room);
            w->len += room;
            off += room;
            if (off < (size_t)n) {
                if (w->sink(w->sink_ctx, w->p, w->len) != 0) {
                    w->sink_aborted = 1;
                } else {
                    w->flushed += w->len;
                    w->len = 0;
                }
            }
        }
        return;
    }
    /* the +1 reserves the terminator slot: the serializers write the
     * NUL at p[len] after the run, and a write landing exactly on cap
     * (len + n == cap) put that NUL one byte past the buffer — the
     * first byte of the next heap block's header (the #182 flake:
     * corrupt deterministic, detected later, timing-varying) */
    if (w->reserve < (size_t)n) {
        wr_reserve(w, n);
        if (w->oom) {
            w->len += n; /* keep counting: the caller fails on oom */
            return;
        }
    }
    if ((size_t)n <= w->reserve) {
        w->reserve -= n;
    } else {
        w->reserve = 0; /* grow=0 trusts the count: the write rides past
                         * the bookkeeping exactly as before */
    }
    memcpy(w->p + w->len, p, n);
    w->len += n;
}

/* #739 lane 1: refill the writable reservation. The grow and flush
 * checks move from every byte to every refill; the watermark law is
 * unchanged in spirit (append-only output — any crossing is a safe
 * cut, a sink just sees chunks up to one reserve past the mark).
 * ONLY the growing routes hold a reservation: the caller-buffer and
 * trusted-count routes keep cap bookkeeping that is not the writer's
 * to reason about (several set cap=0), so their writes stay exactly
 * the old per-call trust. */
static void wr_reserve(yep_writer* w, size_t need) {
    /* the FLUSH runs for every route: the fixed-window streaming route
     * (grow=0) has no reservation, so this refill path is its ONLY
     * flush point -- skipping it let len ride to cap and the next
     * write landed past the window (ASan: heap-buffer-overflow WRITE
     * of size 2 exactly 0 bytes after the 128K region; ubuntu's
     * allocator freed-corrupted, macOS's slack absorbed it). */
    wr_maybe_flush(w);
    if (!w->grow || w->bounded) {
        w->reserve = 0;
        return;
    }
    if (w->oom) {
        return;
    }
    if (w->len + need + 1 > w->cap) { /* +1: the terminator */
        wr_grow(w, need);
        if (w->oom) {
            return;
        }
    }
    w->reserve = w->cap - w->len - 1;
}

static void wr_byte(yep_writer* w, char c) {
    w->last = c;
    if (c == '\n') {
        w->col = 0;
    } else {
        w->col++;
    }
    if (w->dry) {
        w->len++;
        return;
    }
    if (w->reserve == 0) {
        wr_reserve(w, 1);
        if (w->oom) {
            w->len++;
            return;
        }
    }
    if (w->reserve > 0) {
        w->reserve--; /* grow=0 routes keep it at 0: per-call trust */
    }
    w->p[w->len++] = c;
}

static void wr_indent(yep_writer* w, int depth) {
    for (int i = 0; i < depth; i++) {
        wr_byte(w, ' ');
    }
}

/* json_pretty's per-depth indent: the default two spaces per level,
 * or depth copies of the caller's unit (json.h pretty entry). */
static void wr_pretty_indent(yep_writer* w, int depth) {
    if (w->json_indent == NULL) {
        wr_indent(w, depth * 2);
        return;
    }
    for (int i = 0; i < depth; i++) {
        wr_put(w, w->json_indent, (uint32_t)w->json_indent_len);
    }
}

static void emit_dq(yep_writer* w, const char* p, uint32_t n) {
    wr_byte(w, '"');
    uint32_t run = 0;
    for (uint32_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        /* U+0085 (NEL) is a BREAK in YAML: raw it would fold on
         * re-parse. Escape it ('\N' replaces the exact UTF-8 pair, so
         * lengths stay equal). The nightly's fuzz_roundtrip
         * byte-instability crasher, "\"1 \\N\"". */
        if (c == 0xc2 && i + 1 < n && (unsigned char)p[i + 1] == 0x85) {
            if (run) {
                wr_put(w, p + i - run, run);
                run = 0;
            }
            wr_put(w, "\\N", 2);
            i++;
            continue;
        }
        /* LS/PS (U+2028/U+2029) are libyaml breaks: raw, re-parse
         * folds them (the nightly's ";\\P" catch). 2-char escapes of
         * 3-byte sequences: the one-pass dry count sizes them. */
        if (c == 0xe2 && i + 2 < n && (unsigned char)p[i + 1] == 0x80 &&
            ((unsigned char)p[i + 2] == 0xa8 || (unsigned char)p[i + 2] == 0xa9)) {
            if (run) {
                wr_put(w, p + i - run, run);
                run = 0;
            }
            wr_put(w, (unsigned char)p[i + 2] == 0xa8 ? "\\L" : "\\P", 2);
            i += 2;
            continue;
        }
        /* C1 controls (U+0080..U+009F, the UTF-8 pairs c2 80..c2 9f)
         * are NON-PRINTABLE: raw, the re-parse's printability gate
         * rejects the very document we just wrote (the nightly's
         * "\x999" catch — \x99 decodes to U+0099 and the raw pair
         * c2 99 cannot ride). Psych escapes the whole C1 range; the
         * \xNN escape re-enters the doc the same way it left. */
        if (c == 0xc2 && i + 1 < n && (unsigned char)p[i + 1] >= 0x80 &&
            (unsigned char)p[i + 1] <= 0x9f) {
            static const char hd[] = "0123456789ABCDEF";
            if (run) {
                wr_put(w, p + i - run, run);
                run = 0;
            }
            wr_byte(w, '\\');
            wr_byte(w, 'x');
            wr_byte(w, hd[(unsigned char)p[i + 1] >> 4]);
            wr_byte(w, hd[(unsigned char)p[i + 1] & 0xf]);
            i++;
            continue;
        }
        /* U+FFFE/U+FFFF are noncharacters: the printable validator
         * rejects them raw, so emitting them raw makes the document
         * un-re-parseable (the nightly's "["\\uFFFE"]" catch). Their
         * escapes re-enter the doc the same way they left. */
        if (c == 0xef && i + 2 < n && (unsigned char)p[i + 1] == 0xbf &&
            ((unsigned char)p[i + 2] == 0xbe || (unsigned char)p[i + 2] == 0xbf)) {
            if (run) {
                wr_put(w, p + i - run, run);
                run = 0;
            }
            wr_put(w, (unsigned char)p[i + 2] == 0xbe ? "\\uFFFE" : "\\uFFFF", 6);
            i += 2;
            continue;
        }
        /* U+FEFF must never ride raw: at the buffer head it IS the
         * BOM and re-parse would strip it (the nightly's UTF-32LE
         * catch); Psych escapes it everywhere ("﻿") */
        if (c == 0xef && i + 2 < n && (unsigned char)p[i + 1] == 0xbb &&
            (unsigned char)p[i + 2] == 0xbf) {
            if (run) {
                wr_put(w, p + i - run, run);
                run = 0;
            }
            wr_put(w, "\\uFEFF", 6);
            i += 2;
            continue;
        }
        if (c >= 0x20 && c != '"' && c != '\\' && c != 0x7f) {
            run++;
            continue;
        }
        if (run) {
            wr_put(w, p + i - run, run);
            run = 0;
        }
        if (c == '"' || c == '\\') {
            wr_byte(w, '\\');
            wr_byte(w, (char)c);
        } else if (c == '\n') {
            wr_put(w, "\\n", 2);
        } else if (c == '\r') {
            wr_put(w, "\\r", 2);
        } else if (c == '\t') {
            wr_put(w, "\\t", 2);
        } else if (c < 0x20 || c == 0x7f) {
            static const char* named[6] = {"\\0", "\\a", "\\b", "\\v", "\\f", "\\e"};
            static const unsigned char code[6] = {0x00, 0x07, 0x08, 0x0b, 0x0c, 0x1b};
            int k;
            for (k = 0; k < 6; k++) {
                if (code[k] == c) {
                    break;
                }
            }
            if (k < 6) {
                wr_put(w, named[k], 2);
            } else {
                static const char hd[] = "0123456789abcdef";
                char hex[4] = {'\\', 'x', hd[(c >> 4) & 0xf], hd[c & 0xf]};
                wr_put(w, hex, 4);
            }
        }
    }
    if (run) {
        wr_put(w, p + n - run, run);
    }
    wr_byte(w, '"');
}

static void emit_sq(yep_writer* w, const char* p, uint32_t n) {
    wr_byte(w, '\'');
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] == '\'') {
            wr_put(w, "''", 2);
        } else {
            wr_byte(w, p[i]);
        }
    }
    wr_byte(w, '\'');
}

/* JSON string escapes: \" \\ \b \f \n \r \t and \uXXXX for other
 * controls; raw UTF-8 passes through (RFC 8259). */
static void emit_dq_json(yep_writer* w, const char* p, uint32_t n) {
    wr_byte(w, '"');
    static const char* named[32] = {
        NULL, NULL,  NULL,  NULL, NULL, NULL, NULL, NULL, "\\b", "\\t", "\\n",
        NULL, "\\f", "\\r", NULL, NULL, NULL, NULL, NULL, NULL,  NULL,  NULL,
        NULL, NULL,  NULL,  NULL, NULL, NULL, NULL, NULL, NULL,  NULL,
    };
    static const char hd[] = "0123456789abcdef";
    /* run-batched (the #352 emit_dq shape): clean spans ride one
     * wr_put each — per-byte wr_byte calls were 14% of the json
     * write lane's leaf samples (yeptris#739) */
    uint32_t run = 0;
    for (uint32_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c >= 0x20 && c != '"' && c != '\\') {
            run++;
            continue;
        }
        if (run) {
            wr_put(w, p + i - run, run);
            run = 0;
        }
        if (c < 32 && named[c] != NULL) {
            wr_put(w, named[c], 2);
        } else if (c == '"' || c == '\\') {
            wr_byte(w, '\\');
            wr_byte(w, (char)c);
        } else {
            char hex[6] = {'\\', 'u', '0', '0', hd[(c >> 4) & 0xf], hd[c & 0xf]};
            wr_put(w, hex, 6);
        }
    }
    if (run) {
        wr_put(w, p + n - run, run);
    }
    wr_byte(w, '"');
}

/* Literal block: header at the cursor, body lines at parent_col+2.
 * The explicit indicator is always '2' because the pad is defined as
 * parent_col+2 everywhere (the contract that keeps it valid). */
static void emit_literal(yep_writer* w, const yep_dnode* n, int parent_col) {
    yep_view vdec = wv(w, n->value);
    const char* p = (const char*)vdec.p;
    uint32_t len = vdec.len;
    uint32_t trail = 0;
    while (trail < len && p[len - 1 - trail] == '\n') {
        trail++;
    }
    uint32_t body = len - trail;
    wr_byte(w, '|');
    if (trail == 0) {
        wr_byte(w, '-');
    } else if (trail > 1) {
        wr_byte(w, '+');
    }
    /* libyaml's indicator rules (#290 family 4): the explicit indent
     * appears when the first body line starts with a space or is
     * blank — the base indent is unambiguous otherwise (the pad is
     * parent_col+2 by contract; F6MC's leading-blank folded bodies) */
    if (body > 0 && (p[0] == ' ' || p[0] == '\n')) {
        wr_byte(w, '2');
    }
    wr_byte(w, '\n');
    int pad = parent_col + 2;
    wr_indent(w, pad);
    for (uint32_t i = 0; i < body; i++) {
        wr_byte(w, p[i]);
        if (p[i] == '\n' && i + 1 < body) {
            if (p[i + 1] == '\n') {
                continue; /* blank interior lines ride unpadded (libyaml) */
            }
            wr_indent(w, pad);
        }
    }
    /* The keep spelling writes exactly the parsed trailing breaks:
     * written N reads N. (The round-32 extra break for tab-carrying
     * ws-only last lines compensated a READER bug -- the tab-content
     * arm consumed its line's own break without counting it toward
     * trailing -- and the reader counts it now; the compensation made
     * every pass GROW one break.) */
    for (uint32_t i = 0; i < trail; i++) {
        wr_byte(w, '\n');
    }
}

/* Canonical scalar (13B): typed words and numbers plain, everything
 * else double-quoted; floats re-print through the shortest printer so
 * canonical output is a fixed point (parse of canonical re-prints
 * identically — the byte-stability gate). */
static void emit_canonical_scalar(yep_writer* w, const yep_dnode* n) {
    yep_view vdec = wv(w, n->value);
    const char* p = (const char*)vdec.p;
    uint32_t len = vdec.len;
    if (n->tag.len == 0) {
        switch (n->tag_id) {
        case YEPTRIS_TAG_NULL:
            if (w->json) {
                wr_put(w, "null", 4);
            } else {
                wr_put(w, "~", 1);
            }
            return;
        case YEPTRIS_TAG_BOOL:
            if (len == 4) { /* true/TRUE/True/null-ish variants */
                wr_put(w, (p[0] == 't' || p[0] == 'T') ? "true" : "null", 4);
            } else { /* false family (5 chars) */
                wr_put(w, "false", 5);
            }
            return;
        case YEPTRIS_TAG_FLOAT: {
            const char* word = yep_tag_float_word(p, len);
            if (word != NULL) {
                wr_put(w, word, (uint32_t)strlen(word));
                return;
            }
            char buf[40];
            char* end = NULL;
            char tmp[40];
            uint32_t tl = len < sizeof(tmp) - 1 ? len : (uint32_t)sizeof(tmp) - 1;
            memcpy(tmp, p, tl);
            tmp[tl] = '\0';
            double d = strtod(tmp, &end);
            if (end != NULL && *end == '\0' && end != tmp) {
                /* JSON floats carry the json-gem byte shape so
                 * JSON.generate round trips stay byte-identical
                 * (yeptris#651); YAML keeps the shortest shape. */
                int wl = w->json ? yep_d2s_json(d, buf) : yep_d2s_shortest(d, buf);
                wr_put(w, buf, (uint32_t)wl);
                return;
            }
            break; /* unparseable view: fall through to quoting */
        }
        case YEPTRIS_TAG_INT:
            wr_put(w, p, len);
            return;
        default:
            break;
        }
    }
    if (w->json) {
        emit_dq_json(w, p, len);
    } else {
        emit_dq(w, p, len);
    }
}

/* The fidelity-mode emission route for one scalar (#352): the dry
 * pass derives it exactly once; the wet pass consumes the recorded
 * byte and skips the multiline/blockable/safety analysis entirely. */
enum { YEP_SC_NOTHING = 0, YEP_SC_PLAIN, YEP_SC_SQ, YEP_SC_DQ, YEP_SC_LITERAL, YEP_SC_TILDE };
#define YEP_SC_MEMO_MIN 32u /* bytes; derive-below, memoize-above */

/* U+0085 is a break (only its \\N escape is byte-stable), U+FEFF raw is
 * the BOM (re-parse would strip it), and U+2028/29 plus the
 * noncharacters are their twins: content carrying any of these forces
 * the double-quoted route BEFORE the plain fast path can take it (the
 * nightly's fuzz_roundtrip crashers). The NEL scan matches the UTF-8
 * PAIR only — a lone 0x85 is a continuation byte (U+315B = E3 85 9B).
 * The block map's explicit-key promotion consults the same predicate:
 * a key whose content demotes to DQ re-parses as a QUOTED key and must
 * ride simple, or round two's render differs (the nightly's round 118). */
static uint8_t sc_route(yep_writer* w, const char* p, uint32_t len, int as_key, uint8_t sty_in,
                        int has_tag, int no_exotic, int plain_fact) {
    /* the parse-time fact bits (#352 lanes 2-3): a dnode verified clean
     * of the dq-forced set skips that rescan, and a verified plain-safe
     * span skips the safety walk — 0 (mutation-built or JSON classes)
     * keeps the derive */
    if (!no_exotic && yep_scan_dq_forced(p, len)) {
        return YEP_SC_DQ;
    }
    if (w->in_flow && len > 0) {
        /* flow context: the indicators TERMINATE a plain scalar — a
         * raw ']' inside "{}" re-parses as a stray close (the
         * nightly's "555555ta]" explicit-key crasher) */
        for (uint32_t k = 0; k < len; k++) {
            switch (p[k]) {
            case ',':
            case '[':
            case ']':
            case '{':
            case '}':
            case '"':  /* flips the flow scanner into quoted mode */
            case '\\': /* opens an escape sequence there */
            case '\'': /* opens flow single-quote mode */
            case '!':  /* a tag indicator everywhere — "a!%!!a" rode
                        * plain inside a flow-rendered explicit key and
                        * the re-parse died in tag scanning (the
                        * nightly's run 38139449828) */
            case '&':  /* an anchor indicator — "&)J" inside a
                        * flow-rendered explicit key opened an anchor
                        * scan on ')' and the re-parse died ("did not
                        * find expected alphabetic or numeric
                        * character"; the nightly's run 38250474732) */
            case '*':  /* the alias twin of '&' */
            case ':':  /* a separating ':' — "{a: b : c}" died at the
                        * second ':' (the completed-pair law, catch 60);
                        * the nightly's run 38255134508 */
            case '?':  /* the explicit-key twin of ':' */
                return YEP_SC_DQ;
            default:
                break;
            }
        }
    }
    /* the plain verdict is computed AT MOST ONCE: the case-1 arm below
     * re-tests the same bytes when the head check already ran — the
     * #742 profile's second call site (every plain-UNSAFE scalar paid
     * the walk twice) */
    int plain_ok = 0;
    int plain_checked = 0;
    if ((sty_in == 1) && len > 0) {
        plain_ok = plain_fact;
        if (!plain_ok) {
            plain_ok = as_key ? yep_style_plain_key_safe(p, len) : yep_style_plain_safe(p, len);
        }
        plain_checked = 1;
        if (plain_ok) {
            return YEP_SC_PLAIN; /* plain-safe implies no breaks: memchr skipped */
        }
    }
    int multiline = (len > 0 && memchr(p, '\n', len) != NULL);
    int blockable = 0;
    if (multiline && !as_key) {
        int allws = 1;
        int has_cr = 0;
        for (uint32_t i = 0; i < len; i++) {
            if (p[i] != ' ' && p[i] != '\t' && p[i] != '\n' && p[i] != '\r') {
                allws = 0;
            }
            if (p[i] == '\r') {
                has_cr = 1;
            }
        }
        blockable = !allws && !has_cr;
    }
    uint8_t sty = sty_in;
    if (sty == 4 || sty == 5) {
        /* BLOCK SCALARS ARE ILLEGAL IN FLOW: a flow-rendered explicit
         * key's subtree wrote `yaml: |` + literal content lines and
         * the emission could never re-parse (the nightly's run
         * 38154662070, "? !\n- name: ..." — the forced-flow key
         * subtree carried literal members). In flow, literal/folded
         * values ride DQ. */
        if (w->in_flow) {
            return YEP_SC_DQ;
        }
        if (multiline && as_key) {
            return YEP_SC_DQ;
        }
        if (multiline && blockable) {
            return YEP_SC_LITERAL;
        }
        if (multiline) {
            return YEP_SC_DQ;
        }
        if (len == 0) {
            return YEP_SC_DQ;
        }
        /* A TAGGED block scalar keeps the block form even single-line
         * (psych's !binary emits `!binary |-` + one base64 line; an
         * untagged single-line literal re-emits plain — libyaml parity,
         * the #290 family) */
        if (has_tag) {
            return YEP_SC_LITERAL;
        }
        sty = 1;
    }
    switch (sty) {
    case 1:
        if (len == 0) {
            return as_key ? YEP_SC_DQ : YEP_SC_NOTHING;
        }
        if (!plain_checked) {
            plain_ok = plain_fact;
            if (!plain_ok) {
                plain_ok = as_key ? yep_style_plain_key_safe(p, len) : yep_style_plain_safe(p, len);
            }
            plain_checked = 1;
        }
        if (plain_ok) {
            return YEP_SC_PLAIN;
        }
        if (!multiline) {
            for (uint32_t i = 0; i < len; i++) {
                if ((unsigned char)p[i] < 0x20 || (unsigned char)p[i] == 0x7f) {
                    return YEP_SC_DQ;
                }
            }
            return YEP_SC_SQ;
        }
        return YEP_SC_DQ;
    case 2:
        return multiline ? YEP_SC_DQ : YEP_SC_SQ;
    default:
        return YEP_SC_DQ;
    }
}

static void emit_scalar(yep_writer* w, const yep_dnode* n, int parent_col, int as_key) {
    yep_view vdec = wv(w, n->value);
    const char* p = (const char*)vdec.p;
    uint32_t len = vdec.len;
    if (w->canonical || w->json) {
        emit_canonical_scalar(w, n);
        return;
    }
    (void)0;
    uint8_t route;
    /* The memo pays only for LONG scalars: the derive cost scales with
     * len (safety walk, quote scans) while the memo is a fixed store +
     * cold load — on the short scalars of table-shaped documents the
     * memo measured SLOWER than re-deriving (the #357 first artifact:
     * json-users -36%, scalar-heavy +24%). Below the threshold both
     * passes derive; the consumption-order index covers only the
     * memoized long scalars, in visit order either way. */
    if (w->sc_dec != NULL && len >= YEP_SC_MEMO_MIN) {
        if (w->dry) {
            route =
                sc_route(w, p, len, as_key, n->style, n->tag.len > 0, n->no_exotic, n->plain_ok);
            w->sc_dec[w->sc_i] = route;
        } else {
            route = w->sc_dec[w->sc_i];
        }
        w->sc_i++;
    } else {
        route = sc_route(w, p, len, as_key, n->style, n->tag.len > 0, n->no_exotic, n->plain_ok);
    }
    switch (route) {
    case YEP_SC_NOTHING:
        if (w->flow_item) {
            /* a null flow-SEQ item must stay re-parseable: nothing
             * after '[' reads as a leading comma ("[, x]", rejected by
             * libyaml and Psych alike) — libyaml renders it '~' */
            wr_put(w, "~", 1);
            break;
        }
        break; /* an empty plain value emits as nothing (libyaml's null
                  rendering, issue #290) */
    case YEP_SC_PLAIN:
        wr_put(w, p, len);
        break;
    case YEP_SC_SQ:
        emit_sq(w, p, len);
        break;
    case YEP_SC_LITERAL:
        emit_literal(w, n, parent_col);
        break;
    default:
        emit_dq(w, p, len);
        break;
    }
}

/* Canonical mode renames anchors to generated names (a0, a1, ...):
 * exotic source names (flow indicators inside them) cannot survive
 * flow context raw, and renaming preserves the binding semantics.
 * Deterministic by definition order -> byte-stable across roundtrips.
 * The emitter's nametab lives in yep_emitter (owner: yep_emit_run). */
/* An anchor name carrying an indicator byte rides raw fine in block
 * context (the suite's W5VH/2SXE/8XYN round-trip byte-stably), but in
 * FLOW context the bytes flip scanners or terminate the entry — and
 * anchors have no quoting syntax. Such a document has no safe flow
 * spelling: mark it unserializable; serialize fails rather than
 * emitting a form that can never re-parse (the nightly's "&\""/bang/
 * question anchor vein). */
static void wr_anchor_check(yep_writer* w, const yep_view* name) {
    if (!w->in_flow) {
        return;
    }
    for (uint32_t i = 0; i < name->len; i++) {
        switch (name->p[i]) {
        case '"':
        case '\'':
        case '!':
        case '?':
        case '&':
        case '*':
        case ',':
        case '[':
        case ']':
        case '{':
        case '}':
        case '%':
        case '@':
        case '`':
        case '#':
            w->unserializable = 1;
            return;
        default:
            break;
        }
    }
}

/* The anchor-name spell laws, shared by every anchor write path (the
 * node lead AND the line-tail props: the round-45 bonus catch -- a
 * PS-named anchor on a SEQ spelled raw through emit_props_tail and
 * the re-parse died on the normalized break). Returns nonzero when
 * the serialize must fail. */
static int anchor_name_unspellable(yep_writer* w, const yep_dnode* n) {
    yep_view chk = wv(w, n->anchor);
    if (chk.len == 0) {
        return 0;
    }
    if (chk.p[chk.len - 1] == ':') {
        /* a SCALAR keeps riding raw only when it spells INLINE
         * ("k: &n: v" is byte-stable); a BLOCK-style spell puts the
         * header where the separator should be ("&: |" -- the
         * nightly's round 44, crash-07ad9ecc) -- there is no spell:
         * fail the serialize */
        if (n->kind != 0 || n->style == 4 || n->style == 5) {
            w->unserializable = 1;
            return 1;
        }
    }
    /* a NORMALIZE-SUBJECT byte (LS/PS raw, NEL, or a hard break)
     * re-reads as a folded break: pass2 sees a broken/empty name and
     * dies. YAML has no quoted anchor names: fail the serialize. */
    for (size_t ci = 0; ci < chk.len; ci++) {
        unsigned char cb = (unsigned char)chk.p[ci];
        if (cb == '\n' || cb == '\r') {
            w->unserializable = 1;
            return 1;
        }
        if (cb == 0xC2 && ci + 1 < chk.len && (unsigned char)chk.p[ci + 1] == 0x85) {
            w->unserializable = 1; /* NEL */
            return 1;
        }
        if (cb == 0xE2 && ci + 2 < chk.len && (unsigned char)chk.p[ci + 1] == 0x80 &&
            ((unsigned char)chk.p[ci + 2] & 0xFE) == 0xA8) {
            w->unserializable = 1; /* LS/PS */
            return 1;
        }
    }
    return 0;
}

static void emit_anchor_ex(yep_emitter* em, const yep_dnode* n) {
    yep_writer* w = &em->w;
    if (n->anchor.len == 0) {
        return;
    }
    if (anchor_name_unspellable(w, n)) {
        return;
    }
    yep_view adec = wv(w, n->anchor);
    if (w->json) {
        /* JSON spells no anchors — but a name the emitter could never
         * spell still fails the entry (the "{k: &a?b v}" law) */
        wr_anchor_check(w, &adec);
        return;
    }
    wr_byte(w, '&');
    if (w->canonical) {
        uint32_t idx = yep_nametab_get(&em->canon_names, adec);
        if (idx == YEP_NAMETAB_NIL) {
            idx = em->canon_names.count;
            yep_nametab_set(&em->canon_names, adec, idx);
        }
        char name[24];
        int nl = snprintf(name, sizeof(name), "a%u", idx);
        wr_put(w, name, (uint32_t)nl);
    } else {
        wr_anchor_check(w, &adec);
        wr_put(w, (const char*)adec.p, adec.len);
    }
    wr_byte(w, ' ');
}

static void emit_alias_ex(yep_emitter* em, const yep_dnode* n) {
    yep_writer* w = &em->w;
    if (w->json) {
        /* JSON has no alias references: the projection writes null
         * (bounded — dereferencing would loop on cycles) */
        wr_put(w, "null", 4);
        return;
    }
    yep_view name_v = wv(w, n->value);
    if (w->in_flow && !w->canonical) {
        /* the alias twin of wr_anchor_check: a flow alias is spelled
         * raw and the re-parse truncates the name -- a trailing ':'
         * reads as the key/value separator there ("*n:}" names "n",
         * the block definition "n:" went unmatched: the nightly's
         * run 39021984786, "alias references an undefined anchor"
         * 3:15), and quote bytes hit the engine's flow-alias law.
         * Block aliases ride raw (2SXE/Y2GN round-trip byte-stably). */
        wr_anchor_check(w, &name_v);
        if (name_v.len > 0 && name_v.p[name_v.len - 1] == ':') {
            w->unserializable = 1;
        }
    }
    wr_byte(w, '*');
    if (w->canonical) {
        uint32_t idx = yep_nametab_get(&em->canon_names, name_v);
        char name[24];
        int nl;
        if (idx == YEP_NAMETAB_NIL) {
            nl = snprintf(name, sizeof(name), "%.*s", (int)name_v.len, (const char*)name_v.p);
        } else {
            nl = snprintf(name, sizeof(name), "a%u", idx);
        }
        wr_put(w, name, (uint32_t)nl);
    } else {
        wr_put(w, (const char*)name_v.p, name_v.len);
    }
}

/* Tag bytes outside ns-tag-char ride %XX (the parse side decodes them
 * per the URI rule — a decoded newline emitted raw was the nightly's
 * byte-instability crasher "!%0a"). Uppercase hex, libyaml's form. */
static void wr_tag_uri(yep_writer* w, const yep_view* v) {
    static const char hd[] = "0123456789ABCDEF";
    const unsigned char* p = (const unsigned char*)v->p;
    for (uint32_t i = 0; i < v->len; i++) {
        unsigned char c = p[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            c == '-' || c == ';' || c == '/' || c == '?' || c == ':' || c == '@' || c == '&' ||
            c == '=' || c == '+' || c == '$' || c == ',' || c == '.' || c == '!' || c == '~' ||
            c == '*' || c == '\'' || c == '(' || c == ')') {
            wr_byte(w, (char)c);
        } else {
            wr_byte(w, '%');
            wr_byte(w, hd[c >> 4]);
            wr_byte(w, hd[c & 0xf]);
        }
    }
}

/* Explicit tag, verbatim URI form ("!<tag:…> "). Never skipped: a
 * recorded tag always re-emits (implied-by-style skipping made the
 * roundtrip byte-unstable: plain+!!str and ""+!!str collided). */
/* The tag token without separators: the core namespace rides the
 * DEFAULT secondary handle (libyaml writes !!str, never
 * !<tag:yaml.org,2002:str>); a leading-'!' shorthand re-escapes its
 * interior bangs (a local tag's URI can carry them — re-parsed raw
 * they read as handles and the meaning flips); everything else is a
 * verbatim !<URI> wrap. Hosts pass the full core URI — the verbatim
 * shorthand "!!str" as a node tag collides with a LOCAL tag of the
 * same bytes (the binding's chevron-key dump regressed there). */
static void wr_tag_token(yep_writer* w, const yep_view* tdec) {
    static const char k_core_ns[] = "tag:yaml.org,2002:";
    if (tdec->len > sizeof(k_core_ns) - 1 &&
        memcmp(tdec->p, k_core_ns, sizeof(k_core_ns) - 1) == 0) {
        yep_view suffix = {tdec->p + sizeof(k_core_ns) - 1, tdec->len - (sizeof(k_core_ns) - 1)};
        int scan_ok = 1;
        for (uint32_t si = 0; si < suffix.len; si++) {
            if (!yep_scan_tag_char((unsigned char)suffix.p[si])) {
                scan_ok = 0;
                break;
            }
        }
        /* the shorthand only holds if the suffix re-scans: the parse
         * side's tag charset terminates on ',' (and flow indicators —
         * our lenience vs libyaml's URI set), so a suffix carrying one
         * re-parses truncated and the round-trip dies (the nightly's
         * run 38132505901, the verbatim tag with an interior ',');
         * the verbatim !<...> wrap carries those */
        if (scan_ok) {
            wr_put(w, "!!", 2);
            wr_tag_uri(w, &suffix);
            return;
        }
    }
    if (tdec->len > 0 && tdec->p[0] == '!') {
        yep_view rest = {tdec->p + 1, tdec->len - 1};
        /* the LOCAL shorthand re-scans too: the parse side's tag
         * charset terminates on ',' (our lenience vs libyaml's URI
         * set), so a suffix carrying one re-parses truncated — the
         * same law the core-namespace arm gained (catch 47); the
         * verbatim !<...> wrap carries those (the nightly's run
         * 38165476712, "!<!<<b,,,,,-,,,,ar> :yam" emitted
         * "!%3C%3Cb,,,,,-,,,,ar" and died at the first comma) */
        int scan_ok = 1;
        for (uint32_t si = 0; si < rest.len; si++) {
            if (!yep_scan_tag_char((unsigned char)rest.p[si])) {
                scan_ok = 0;
                break;
            }
        }
        if (scan_ok) {
            wr_byte(w, '!');
            uint32_t run = 0;
            for (uint32_t i = 0; i < rest.len; i++) {
                if (rest.p[i] == '!') {
                    if (run > 0) {
                        yep_view seg = {rest.p + i - run, run};
                        wr_tag_uri(w, &seg);
                        run = 0;
                    }
                    wr_put(w, "%21", 3);
                } else {
                    run++;
                }
            }
            if (run > 0) {
                yep_view seg = {rest.p + rest.len - run, run};
                wr_tag_uri(w, &seg);
            }
            return;
        }
    }
    wr_byte(w, '!');
    wr_byte(w, '<');
    wr_tag_uri(w, tdec);
    wr_byte(w, '>');
}

static void emit_tag(yep_writer* w, const yep_dnode* n) {
    if (n->tag.len == 0) {
        return;
    }
    if (w->json) {
        return; /* JSON spells no tags (the ts probe: "k: !" -> {"k": ! ""}) */
    }
    yep_view tdec = wv(w, n->tag);
    wr_tag_token(w, &tdec);
    wr_byte(w, ' ');
}

/* Line-tail properties for block collections: "!<t> &a" after
 * "key:"/"-" — properties may not precede a dash entry directly (the
 * suite's SY6V rule). */
static void emit_props_tail(yep_writer* w, const yep_dnode* n) {
    if (n->anchor.len > 0 && anchor_name_unspellable(w, n)) {
        return;
    }
    if (w->json) {
        return; /* JSON spells no properties */
    }
    if (n->tag.len > 0) {
        wr_byte(w, ' ');
        yep_view tdec2 = wv(w, n->tag);
        wr_tag_token(w, &tdec2);
    }
    if (n->anchor.len > 0) {
        wr_byte(w, ' ');
        yep_view adec2 = wv(w, n->anchor);
        wr_anchor_check(w, &adec2);
        wr_byte(w, '&');
        wr_put(w, (const char*)adec2.p, adec2.len);
    }
}

static void emit_node(yep_emitter* em, uint32_t id, int parent_col, int as_key);

/* The simple-key law counts the WRITTEN token. A quoted render
 * expands escapes (a '"' doubles, controls ride \xNN, non-ASCII may);
 * raw-length predicates let escape-heavy scalars through (the
 * nightly's run 38145696566: a ~1000-raw key rendering ~3000
 * written). Conservative factor-4 estimate for every expandable
 * byte — overestimating only takes the (always-legal) explicit form. */
static int key_written_over_limit(yep_writer* w, const yep_dnode* kn) {
    if (w->json) {
        return 0; /* JSON has no simple-key law: every key writes (#739) */
    }
    yep_view kv_ = wv(w, kn->value);
    if (kn->anchor.len == 0 && kn->tag.len == 0 && kv_.len <= 255) {
        /* the exact bound: even at the factor-4 expansion the walk
         * budgets per byte, 255 raw bytes top out at est = 2 + 1020 —
         * under the law. Table-shaped documents pay the two field
         * checks instead of the byte walk (the #742 profile's 22%
         * lane) */
        return 0;
    }
    uint64_t est = 2; /* the quotes */
    const unsigned char* q = (const unsigned char*)kv_.p;
    uint32_t n = kv_.len;
    /* NO early return on an empty value: the tag arm below must run —
     * a TAGGED EMPTY key writes ~2300 bytes of escaped tag and the
     * re-parse's simple-key law rejects it ("the nightly's run
     * 38200627503") */
    if (q != NULL) {
        for (uint32_t i = 0; i < n; i++) {
            /* a single-quoted render DOUBLES every apostrophe; without
             * it the estimate missed a 892-char key carrying 137
             * quotes (rendered 1031, re-parse rejected the simple
             * key — the nightly's crash-b8a18e43) */
            est += (q[i] < 0x20 || q[i] >= 0x7F || q[i] == '"' || q[i] == '\\' || q[i] == '\'')
                       ? 4u
                       : 1u;
        }
    }
    if (kn->anchor.len > 0) {
        /* the key's ANCHOR rides the same written token: the re-parse
         * counts the '&name' bytes in the simple-key candidate, and
         * anchor names carry interior colons (Y2GN) — a ~1700-byte
         * name blew a 2-byte key past the law (the nightly's run
         * 38206305189) */
        est += (uint64_t)kn->anchor.len + 2u;
    }
    if (kn->tag.len > 0) {
        /* the key's TAG rides the same written token: wr_tag_uri
         * escapes every byte outside its URI set as %XX (even '_'),
         * inflating a ~1000-byte tag past the re-parse's simple-key
         * law (the nightly's run 38176016091: a ~975-raw tag wrote
         * ~1030 and the emission never re-parsed) */
        yep_view tv = wv(w, kn->tag);
        const unsigned char* t = (const unsigned char*)tv.p;
        for (uint32_t i = 0; i < tv.len; i++) {
            unsigned char c = t[i];
            int plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') || c == '-' || c == ';' || c == '/' || c == '?' ||
                        c == ':' || c == '@' || c == '&' || c == '=' || c == '+' || c == '$' ||
                        c == ',' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' ||
                        c == '(' || c == ')';
            est += plain ? 1u : 3u;
        }
        est += 6u; /* the !!/!< > wrapper and the separating space */
    }
    return est > 1024u;
}

static void emit_flow(yep_emitter* em, uint32_t id) {
    yep_writer* w = &em->w;
    int saved_flow = w->in_flow;
    w->in_flow = 1;
    const yep_dom* d = em->doc->dom;
    const yep_dnode* n = yep_dom_node(d, id);
    emit_tag(w, n);
    emit_anchor_ex(em, n);
    int map = (n->kind == 2);
    wr_byte(w, map ? '{' : '[');
    int flow_indent = w->col;
    if (w->json_pretty && n->count > 0) {
        w->pretty_depth++;
        wr_byte(w, '\n');
        wr_pretty_indent(w, w->pretty_depth);
        flow_indent = w->col;
    }
    uint32_t child = n->first_child;
    uint32_t idx = 0;
    while (child != UINT32_MAX) {
        const yep_dnode* cn = yep_dom_node(d, child);
        if (idx > 0) {
            /* 13B: past best_width, break AFTER the previous value
             * (value boundaries only, never inside a scalar) and
             * re-indent to the opening bracket's column */
            if (w->json_pretty) {
                wr_put(w, ",\n", 2);
                wr_pretty_indent(w, w->pretty_depth);
            } else if (w->json_compact) {
                wr_byte(w, ',');
            } else if (w->best_width > 0 && w->col > w->best_width) {
                wr_byte(w, ',');
                wr_byte(w, '\n');
                wr_indent(w, flow_indent);
            } else {
                wr_put(w, ", ", 2);
            }
        }
        if (map) {
            int explicit_key = cn->anchor.len > 0 || cn->tag.len > 0 || cn->kind != 0;
            if (!explicit_key && key_written_over_limit(w, cn)) {
                { /* the quoted render's span vs the simple-key law */
                    /* the simple-key law: a >1024 key cannot ride as a
                     * flow simple key (the re-parse rejects it) — the
                     * explicit "? key : " form carries it. Strict JSON
                     * accepts keys of any length, so the JSON route
                     * parses them and the YAML emission must re-parse
                     * (the nightly's run 38143486175) */
                    explicit_key = 1;
                }
            }
            if (explicit_key) {
                wr_put(w, "? ", 2);
                emit_node(em, child, 0, 1);
                wr_put(w, " : ", 3);
            } else {
                emit_node(em, child, 0, 1);
                if (w->json_pretty) {
                    wr_put(w, ": ", 2);
                } else if (w->json_compact) {
                    wr_byte(w, ':');
                } else {
                    wr_put(w, ": ", 2);
                }
            }
            emit_node(em, cn->next_sibling, 0, 0);
            child = cn->next_sibling;
            cn = yep_dom_node(d, child);
        } else {
            w->flow_item = 1; /* a null item must ride '~' */
            emit_node(em, child, 0, 0);
            w->flow_item = 0;
        }
        idx++;
        child = cn->next_sibling;
    }
    if (w->json_pretty && idx > 0) {
        wr_byte(w, '\n');
        wr_pretty_indent(w, w->pretty_depth - 1);
        w->pretty_depth--;
    }
    wr_byte(w, map ? '}' : ']');
    w->in_flow = saved_flow;
}

/* Block map: first key at the cursor (the caller placed it at
 * content_col = parent_col+2); following keys on new lines there. */
static void emit_block_map(yep_emitter* em, uint32_t id, int content_col) {
    yep_writer* w = &em->w;
    const yep_dom* d = em->doc->dom;
    const yep_dnode* n = yep_dom_node(d, id);
    uint32_t child = n->first_child;
    uint32_t pair = 0;
    while (child != UINT32_MAX) {
        /* #352: the flat plain member specialist — tiny scalar pairs
         * pay dispatch and props checks, not byte work. Strictly
         * narrower than the general body below: plain unpropertied
         * key, plain unpropertied scalar value, both safety-proven by
         * the exact predicates sc_route consults (no memo touch, so
         * the dry/wet sc_i streams stay aligned). Anything else falls
         * through untouched — parity by construction. */
        const yep_dnode* kn0 = yep_dom_node(d, child);
        const yep_dnode* vn0 = yep_dom_node(d, kn0->next_sibling);
        if (!w->canonical && !w->json && !w->force_flow && vn0 != NULL && kn0->kind == 0 &&
            kn0->style == 1 && kn0->anchor.len == 0 && kn0->tag.len == 0 && kn0->count == 0 &&
            vn0->kind == 0 && vn0->style == 1 && vn0->anchor.len == 0 && vn0->tag.len == 0 &&
            vn0->count == 0) {
            yep_view kv = wv(w, kn0->value);
            yep_view vv = wv(w, vn0->value);
            uint32_t f_ind = 0, f_nl = 0, f_total = 0;
            int fused = kv.len > 0 && !key_written_over_limit(w, kn0) && kn0->plain_ok &&
                        (vv.len == 0 || vn0->plain_ok);
            if (fused) {
                /* fused member write: plain-safe spans carry no breaks,
                 * so the col scan is arithmetic and one reservation
                 * covers the whole member (line + key + ':' + value) */
                f_ind = pair > 0 ? (uint32_t)content_col : 0u;
                f_nl = (pair > 0 && w->last != '\n') ? 1u : 0u;
                f_total = f_nl + f_ind + kv.len + 1u + (vv.len > 0 ? vv.len + 1u : 0u);
                /* the fixed-window streaming route: the fused write
                 * bypasses wr_put (whose chunked path bounds every
                 * put), and this block ends in continue — so a member
                 * that does not fit the window must DECLINE THE WHOLE
                 * ARM to reach the general body, which emits the
                 * identical bytes through wr_put. cap==0 routes are the
                 * old trusted-count forms: the writer cannot reason
                 * about their capacity, and they keep the arm. */
                if (!w->dry && !w->grow && w->cap != 0 && w->len + (size_t)f_total + 1 > w->cap) {
                    fused = 0;
                }
            }
            if (fused) {
                w->last = vv.len > 0 ? (char)vv.p[vv.len - 1] : ':';
                if (f_nl) {
                    w->col = (int)(f_total - 1u); /* the break resets, then the line */
                } else {
                    w->col += (int)f_total;
                }
                if (w->dry) {
                    w->len += f_total;
                } else {
                    if (w->grow && w->len + f_total + 1 > w->cap) {
                        if (w->bounded) {
                            w->oom = 1; /* keep counting: the trusted caller reads the size */
                        } else {
                            wr_grow(w, f_total);
                        }
                    }
                    if (w->oom) {
                        w->len += f_total; /* keep counting: the caller fails on oom */
                    } else {
                        /* the fused write bypasses wr_put/wr_byte: keep
                         * the reservation honest (the #739 lane-1 law) */
                        w->reserve =
                            (size_t)f_total <= w->reserve ? w->reserve - (size_t)f_total : 0;
                        char* q = w->p + w->len;
                        if (f_nl) {
                            *q++ = '\n';
                        }
                        for (uint32_t i = 0; i < f_ind; i++) {
                            *q++ = ' ';
                        }
                        memcpy(q, kv.p, kv.len);
                        q += kv.len;
                        *q++ = ':';
                        if (vv.len > 0) {
                            *q++ = ' ';
                            memcpy(q, vv.p, vv.len);
                        }
                        w->len += f_total;
                        wr_maybe_flush(w);
                    }
                }
                pair++;
                child = vn0->next_sibling;
                continue;
            }
        }
        const yep_dnode* kn = yep_dom_node(d, child);
        const yep_dnode* vn = yep_dom_node(d, kn->next_sibling);
        if (pair > 0) {
            if (w->last != '\n') {
                wr_byte(w, '\n'); /* keep-chomped bodies own their break */
            }
            wr_indent(w, content_col);
        }
        int complex_key = (kn->kind != 0);
        /* a literal/folded key cannot ride the simple form: the value
         * separator would glue onto the block's CONTENT line (colons
         * are data there) and the re-parse reads a different tree —
         * the explicit "? key" form carries it. An EMPTY block-styled
         * key rides simple: it has no content line for the separator
         * to glue onto, and sc_route demotes it to the quoted form
         * anyway (the nightly's run 38177622733, ":\n! |-\n r\n:") */
        if (!complex_key && kn->style >= 4 && kn->style <= 5 && kn->value.len > 0 &&
            kn->tag.len > 0 && memchr(wv(w, kn->value).p, '\n', wv(w, kn->value).len) == NULL &&
            (kn->no_exotic || !yep_scan_dq_forced(wv(w, kn->value).p, wv(w, kn->value).len))) {
            /* a TAGGED SINGLE-LINE literal/folded key keeps the block
             * form in sc_route (the #290 law), so the value separator
             * would glue onto the block's CONTENT line (colons are
             * data there) and the re-parse reads a different tree —
             * the explicit "? key" form carries it, and it keeps the
             * block form there too, so both renders agree. A MULTILINE
             * tagged key demotes to quoted even in the explicit path
             * (as_key), so promoting it would UNSTABILIZE it: round
             * two parses a quoted style and rides simple. Untagged and
             * empty block styles demote likewise (the nightly's runs
             * 38177622733 ":\n! |-\n r\n:" and 38181819580
             * ":\n! |\n r\n:") — and so does content carrying
             * NEL/BOM/LS/PS/noncharacters (sc_dq_forced): a BOM in a
             * key's content demoted the explicit form's key to DQ and
             * round two rode simple, one render behind (the nightly's
             * round 118) */
            complex_key = 1;
        }
        if (!complex_key && key_written_over_limit(w, kn)) {
            /* the simple-key law, block side: a >1024 key cannot ride
             * as a block simple key either — the explicit "? key" form
             * carries it (the flow member writer gained the same law;
             * the nightly's run 38143486175) */
            complex_key = 1;
        }
        if (complex_key) {
            w->force_flow = 1; /* collections as flow keys: compact and
                                  free of the block '?' layout edges */
            wr_put(w, "? ", 2);
            /* the props emit ONCE: the DQ-promoted path writes its own
             * (emit_dq is bare), everything else lets emit_node/
             * emit_flow carry tag+anchor inline — the pre-emit made
             * every non-DQ explicit key re-emit its props ("? !\n  ! |-
             " — the catch-56 law, explicit-key side) */
            int dq_promoted = (kn->kind == 0 && key_written_over_limit(w, kn));
            if (dq_promoted) {
                emit_props_tail(w, kn);
                if (kn->anchor.len > 0 || kn->tag.len > 0) {
                    wr_byte(w, '\n');
                    wr_indent(w, content_col + 2);
                }
            }
            if (dq_promoted) {
                /* the 1024-promoted SCALAR key must ride QUOTED in the
                 * explicit form: plain, its interior ': ' sequences
                 * re-parse through the inline simple-key promotion and
                 * die on the very law that forced the promotion (the
                 * nightly's run 38145696566, the "? licit k:[...]"
                 * emission) */
                emit_dq(w, (const char*)wv(w, kn->value).p, (uint32_t)wv(w, kn->value).len);
            } else {
                emit_node(em, child, content_col, 1);
            }
            wr_byte(w, '\n');
            wr_indent(w, content_col);
            wr_put(w, ": ", 2);
            w->force_flow = 0;
            {
                /* the value's props ride the ':' line and a nested
                 * block collection takes the fresh-line form, exactly
                 * like the simple arm's path: the explicit twin
                 * skipped both, so an anchored map value lost its
                 * anchor (or glued it to the first key) and a later
                 * alias emitted a dangling reference (the nightly's
                 * run 38371134997, "? &d!se: &base\n   name: Esame"
                 * with a complex key forcing the explicit form) */
                if (!(vn->kind == 0 || vn->kind == 3 || vn->flow || vn->count == 0)) {
                    int seq_val = (vn->kind == 1 && !vn->flow);
                    int val_col = seq_val ? content_col : content_col + 2;
                    emit_props_tail(w, vn);
                    wr_byte(w, '\n');
                    wr_indent(w, val_col);
                    emit_node(em, kn->next_sibling, val_col - 2, 0);
                } else {
                    emit_node(em, kn->next_sibling, content_col, 0);
                }
            }
        } else {
            emit_tag(w, kn);
            emit_anchor_ex(em, kn);
            emit_scalar(w, kn, content_col, 1);
            wr_byte(w, ':');
            /* empty collections ride the key line flow (k: [] / k: {})
             * like libyaml — a block-empty has no valid next-line form at
             * the seq indent (an empty [] at the key's column is not a
             * legal value; pyyaml rejects it, issue #52's differential).
             * An implicit-empty plain value rides bare (k:) with no
             * trailing space — libyaml's null rendering (#290) */
            if (vn->kind == 0 || vn->kind == 3 || vn->flow || vn->count == 0) {
                int empty_plain = vn->kind == 0 && vn->style == 1 && vn->anchor.len == 0 &&
                                  vn->tag.len == 0 && wv(w, vn->value).len == 0 && !w->canonical &&
                                  !w->json;
                if (!empty_plain) {
                    wr_byte(w, ' ');
                }
                emit_node(em, kn->next_sibling, content_col, 0);
            } else {
                /* nested block collection: next line. libyaml parity:
                 * a SEQUENCE value sits at the key's column (zero
                 * extra indent); mappings indent +2 (issue #95) */
                int seq_val = (vn->kind == 1 && !vn->flow);
                int val_col = seq_val ? content_col : content_col + 2;
                emit_props_tail(w, vn);
                wr_byte(w, '\n');
                wr_indent(w, val_col);
                /* emit_block_seq dashes at parent_col+2: pass val_col-2
                 * so the dashes land at val_col exactly */
                emit_node(em, kn->next_sibling, val_col - 2, 0);
            }
        }
        pair++;
        child = vn->next_sibling;
    }
}

/* Block seq: "- item" lines at content_col = parent_col+2. */
static void emit_block_seq(yep_emitter* em, uint32_t id, int content_col) {
    yep_writer* w = &em->w;
    const yep_dom* d = em->doc->dom;
    const yep_dnode* n = yep_dom_node(d, id);
    uint32_t child = n->first_child;
    uint32_t idx = 0;
    while (child != UINT32_MAX) {
        const yep_dnode* cn = yep_dom_node(d, child);
        if (idx > 0) {
            if (w->last != '\n') {
                wr_byte(w, '\n'); /* keep-chomped bodies own their break */
            }
            wr_indent(w, content_col);
        }
        if (cn->kind == 0 && cn->style == 1 && cn->anchor.len == 0 && cn->tag.len == 0 &&
            wv(w, cn->value).len == 0 && !w->canonical && !w->json) {
            wr_put(w, "-\n", 2); /* libyaml: a null item is a bare dash */
            idx++;
            child = cn->next_sibling;
            continue;
        }
        if (cn->kind != 0 && cn->kind != 3 && !cn->flow && cn->count > 0 &&
            (cn->anchor.len > 0 || cn->tag.len > 0)) {
            /* props may not precede a dash: they ride the dash line and
             * the collection starts on the next line (SY6V). The dash
             * carries NO trailing space — emit_props_tail adds its own
             * separator, and "- " + " &a" doubled it (the nightly's run
             * 38161738783). An EMPTY propertied collection takes the
             * compact arm: emit_node's empty-form writes the props
             * itself ("- &a {}"), and the next-line form re-emitted
             * them ("- &a\n  &a {}") — every round grew/duplicated */
            wr_byte(w, '-');
            emit_props_tail(w, cn);
            wr_byte(w, '\n');
            wr_indent(w, content_col + 2);
            emit_node(em, child, content_col, 0);
        } else {
            /* compact: a block collection starts right after "- " */
            wr_put(w, "- ", 2);
            emit_node(em, child, content_col, 0);
        }
        idx++;
        child = cn->next_sibling;
    }
}

static void emit_node(yep_emitter* em, uint32_t id, int parent_col, int as_key) {
    yep_writer* w = &em->w;
    const yep_dom* d = em->doc->dom;
    const yep_dnode* n = yep_dom_node(d, id);
    if (n == NULL) {
        return;
    }
    if (n->kind == 3) { /* alias */
        emit_alias_ex(em, n);
        return;
    }
    if ((n->kind == 1 || n->kind == 2) &&
        (n->flow || w->in_flow || w->canonical || w->json || w->force_flow)) {
        /* force_flow holds through the subtree: a complex key's nested
         * collections all render flow (scalars are never collections).
         * in_flow is the catch-55 law's collection arm: a block-styled
         * node nested inside a flow member renders FLOW — its block
         * dashes are not node content there (the nightly's run
         * 38168927841) */
        emit_flow(em, id);
        return;
    }
    if (n->kind == 2) { /* mapping */
        if (n->count == 0) {
            emit_tag(w, n);
            emit_anchor_ex(em, n);
            wr_put(w, "{}", 2);
        } else {
            emit_block_map(em, id, parent_col + 2);
        }
        return;
    }
    if (n->kind == 1) { /* sequence */
        if (n->count == 0) {
            emit_tag(w, n);
            emit_anchor_ex(em, n);
            wr_put(w, "[]", 2);
        } else {
            emit_block_seq(em, id, parent_col + 2);
        }
        return;
    }
    emit_tag(w, n);
    emit_anchor_ex(em, n);
    if (parent_col < 0) {
        parent_col = 0; /* root scalar: block body at column 2 */
    }
    emit_scalar(w, n, parent_col, as_key);
}

/* ============================================================
 * yeptris#739 slice 3: JSON's own tree walk. In JSON mode every
 * collection is flow, the separators are fixed, and the YAML
 * machinery (props lines, style analysis, explicit-key law,
 * pretty depth) never fires — yet the generic emit_node ->
 * emit_flow path re-tests all of it per node and looks every
 * child up twice. This walk emits the same bytes with one
 * dispatch per node and two per map pair. Pretty output keeps
 * the generic path: its depth/indent bookkeeping is the point
 * there.
 * ============================================================ */
static void emit_json_node(yep_emitter* em, uint32_t id);

static void emit_json_flow(yep_emitter* em, uint32_t id) {
    yep_writer* w = &em->w;
    int saved_flow = w->in_flow;
    w->in_flow = 1;
    const yep_dom* d = em->doc->dom;
    const yep_dnode* n = yep_dom_node(d, id);
    if (n->tag.len > 0) {
        emit_tag(w, n);
    }
    if (n->anchor.len > 0) {
        emit_anchor_ex(em, n);
    }
    int map = (n->kind == 2);
    wr_byte(w, map ? '{' : '[');
    uint32_t child = n->first_child;
    uint32_t idx = 0;
    while (child != UINT32_MAX) {
        const yep_dnode* cn = yep_dom_node(d, child);
        if (idx > 0) {
            if (w->json_compact) {
                wr_byte(w, ',');
            } else {
                wr_put(w, ", ", 2);
            }
        }
        if (map) {
            /* the one generic-path rule JSON keeps: a non-scalar key
             * rides the explicit "? key : " form — same bytes emit_flow
             * writes. (Propertied scalar keys used to join it; JSON
             * drops properties, so they ride the plain quoted form.) */
            if (cn->kind != 0) {
                wr_put(w, "? ", 2);
                emit_json_node(em, child);
                wr_put(w, " : ", 3);
            } else {
                emit_json_node(em, child);
                if (w->json_compact) {
                    wr_byte(w, ':');
                } else {
                    wr_put(w, ": ", 2);
                }
            }
            uint32_t val = cn->next_sibling;
            emit_json_node(em, val);
            const yep_dnode* vn = yep_dom_node(d, val);
            child = vn != NULL ? vn->next_sibling : UINT32_MAX;
        } else {
            emit_json_node(em, child);
            child = cn->next_sibling;
        }
        idx++;
    }
    wr_byte(w, map ? '}' : ']');
    w->in_flow = saved_flow;
}

static void emit_json_node(yep_emitter* em, uint32_t id) {
    yep_writer* w = &em->w;
    const yep_dom* d = em->doc->dom;
    const yep_dnode* n = yep_dom_node(d, id);
    if (n == NULL) {
        return;
    }
    if (n->kind == 3) { /* alias */
        emit_alias_ex(em, n);
        return;
    }
    if (n->kind == 1 || n->kind == 2) {
        emit_json_flow(em, id);
        return;
    }
    if (n->tag.len > 0) {
        emit_tag(w, n);
    }
    if (n->anchor.len > 0) {
        emit_anchor_ex(em, n);
    }
    emit_canonical_scalar(w, n);
}

/* ============================================================
 * yeptris-ruby#322: the marshal READER — Ruby Marshal.dump bytes
 * to JSON text in one C call (no per-scalar crossings, no object
 * reconstruction). The format facts mirror marshal.c's emission
 * contract (empirically pinned against Ruby 3.4): w_long small form
 * x==0 -> 0x00, |x| <= 122 -> sign*(|x|+5), size form tag +-k with
 * LE payload (2^(8k) - m for negatives); strings carry 'I' ivar
 * wrappers; arrays/hashes/strings/floats register for '@' links.
 *
 * '@' links MEMMOVE the registered object's emitted span — the first
 * walk's bytes are the cache; a lookup miss is a cycle (JSON cannot
 * express it, the binding falls back). No re-parse, no registration
 * on links: the object table stays exactly Ruby's stream order.
 *
 * The JSON-irrelevant constructs ('o' objects, 'S' structs, 'u'
 * user marshal, 'l' bignums beyond i64, non-finite floats) return
 * UNSUPPORTED — the binding falls back to its per-node walk.
 * ============================================================ */

typedef struct {
    const char* p;
    size_t len;
    size_t at;
} mj_stream;

typedef struct {
    const char* p;
    uint32_t len;
} mj_sym;

typedef struct {
    size_t pos;     /* the stream position of the type byte */
    size_t out_off; /* where the emitted span starts in w's buffer */
    size_t out_len; /* the span's length */
} mj_obj;

typedef struct {
    yep_writer* w;
    mj_stream s;
    mj_sym* syms;
    size_t syms_n, syms_cap;
    mj_obj* objs;
    size_t objs_n, objs_cap;
    int failed; /* 0 ok, YEPTRIS_ERROR_UNSUPPORTED */
    int compact;
    int no_reg; /* the next mj_value entry skips registration — an
                 * 'I'-wrapped construct registers ONCE (at the 'I');
                 * its immediate inner must not add a second entry or
                 * every later '@' link shifts */
    int depth;  /* mj_value recursion guard — the reader walks hostile
                 * bytes, not Ruby's heap: nesting is unbounded in the
                 * input, the stack is not (the parser's own
                 * max_depth=1000 is the same law) */
} mj_ctx;

static int mj_fail(mj_ctx* c) {
    c->failed = YEPTRIS_ERROR_UNSUPPORTED;
    return -1;
}

static int mj_byte(mj_ctx* c, unsigned char* out) {
    if (c->s.at >= c->s.len)
        return mj_fail(c);
    *out = (unsigned char)c->s.p[c->s.at++];
    return 0;
}

/* the w_long inverse — mirrors m_w_long's encode exactly:
 *   0x00 -> 0; 1..4 -> positive size form k=b; 5..127 -> small +(
 *   b-5); 129..250 -> small -(251-b); 251..255 -> negative size form
 *   k=256-b (payload = LE(2^(8k) - m)); 128 unreachable. */
static int mj_long(mj_ctx* c, int64_t* out) {
    unsigned char b;
    if (mj_byte(c, &b) != 0)
        return -1;
    if (b == 0) {
        *out = 0;
        return 0;
    }
    if (b <= 4) { /* positive size form, k = b */
        int64_t v = 0;
        for (int i = 0; i < (int)b; i++) {
            unsigned char d;
            if (mj_byte(c, &d) != 0)
                return -1;
            v |= (int64_t)d << (8 * i);
        }
        *out = v;
        return 0;
    }
    if (b <= 127) {
        *out = (int64_t)b - 5;
        return 0;
    }
    if (b >= 129 && b <= 250) {
        *out = (int64_t)b - 251;
        return 0;
    }
    { /* 251..255: negative size form */
        int bytes = 256 - (int)b;
        int64_t v = 0;
        for (int i = 0; i < bytes; i++) {
            unsigned char d;
            if (mj_byte(c, &d) != 0)
                return -1;
            v |= (int64_t)d << (8 * i);
        }
        *out = v - ((int64_t)1 << (8 * bytes));
        return 0;
    }
}

static int mj_ivar_pairs(mj_ctx* c);
static int mj_value(mj_ctx* c);
static int mj_discard_value(mj_ctx* c);

static int mj_string_body(mj_ctx* c, int64_t len) {
    if (len < 0 || (uint64_t)len > (uint64_t)(c->s.len - c->s.at))
        return mj_fail(c);
    emit_dq_json(c->w, c->s.p + c->s.at, (uint32_t)len);
    c->s.at += (size_t)len;
    return 0;
}

/* a symbol: ':' + long(len) + bytes — registers in the table; the
 * name is emitted as a JSON string when emit is set */
static int mj_symbol(mj_ctx* c, int emit) {
    int64_t len;
    if (mj_long(c, &len) != 0)
        return -1;
    if (len < 0 || (uint64_t)len > (uint64_t)(c->s.len - c->s.at))
        return mj_fail(c);
    const char* name = c->s.p + c->s.at;
    uint32_t nlen = (uint32_t)len;
    c->s.at += (size_t)len;
    if (c->syms_n == c->syms_cap) {
        c->syms_cap = c->syms_cap ? c->syms_cap * 2 : 16;
        c->syms = (mj_sym*)realloc(c->syms, c->syms_cap * sizeof(*c->syms));
        if (!c->syms)
            return mj_fail(c);
    }
    c->syms[c->syms_n].p = name;
    c->syms[c->syms_n].len = nlen;
    c->syms_n++;
    if (emit)
        emit_dq_json(c->w, name, nlen);
    return 0;
}

/* the count + (symbol, value) pairs that follow an 'I' wrapper's
 * wrapped value. Parsed and DISCARDED — encoding/freeze ivars have
 * no JSON spelling; the symbol still enters the ';' table. */
static int mj_ivar_pairs(mj_ctx* c) {
    int64_t count;
    if (mj_long(c, &count) != 0)
        return -1;
    for (int64_t i = 0; i < count; i++) {
        unsigned char nt;
        if (mj_byte(c, &nt) != 0)
            return -1;
        if (nt == ':') {
            if (mj_symbol(c, 0) != 0)
                return -1; /* the name: tabled, not emitted */
        } else if (nt == ';') {
            int64_t sidx;
            if (mj_long(c, &sidx) != 0)
                return -1;
            if (sidx < 0 || (size_t)sidx >= c->syms_n)
                return mj_fail(c);
        } else {
            return mj_fail(c); /* ivar names are symbols only */
        }
        if (mj_discard_value(c) != 0)
            return -1; /* the ivar value */
    }
    return 0;
}

/* stream-position-only walk: consumes one value, emits nothing */

#define MJ_DEPTH_MAX 1000

/* the discard twin rides the same budget (its 'I'/'['/'{' recursion is
 * just as hostile-byte reachable) */
static int mj_discard_value_inner(mj_ctx* c);

static int mj_discard_value(mj_ctx* c) {
    if (c->depth >= MJ_DEPTH_MAX)
        return mj_fail(c);
    c->depth++;
    int rc = mj_discard_value_inner(c);
    c->depth--;
    return rc;
}

static int mj_discard_value_inner(mj_ctx* c) {
    unsigned char t;
    int rc = mj_byte(c, &t);
    if (rc != 0)
        return -1;
    switch (t) {
    case '0':
    case 'T':
    case 'F':
        return 0;
    case 'i':
    case ';':
    case '@': {
        int64_t v;
        return mj_long(c, &v);
    }
    case 'l': {
        unsigned char sign;
        int64_t nlimbs;
        if (mj_byte(c, &sign) != 0)
            return -1;
        if (sign != 0x2b && sign != 0x2d)
            return mj_fail(c);
        if (mj_long(c, &nlimbs) != 0)
            return -1;
        if (nlimbs < 0 || (uint64_t)(nlimbs * 2) > (uint64_t)(c->s.len - c->s.at))
            return mj_fail(c);
        c->s.at += (size_t)(nlimbs * 2);
        return 0;
    }
    case 'f':
    case '"': {
        int64_t len;
        if (mj_long(c, &len) != 0)
            return -1;
        if (len < 0 || (uint64_t)len > (uint64_t)(c->s.len - c->s.at))
            return mj_fail(c);
        c->s.at += (size_t)len;
        return 0;
    }
    case 'I':
        if (mj_discard_value(c) != 0)
            return -1;
        return mj_ivar_pairs(c);
    case ':':
        return mj_symbol(c, 0);
    case '[': {
        int64_t count;
        if (mj_long(c, &count) != 0)
            return -1;
        for (int64_t i = 0; i < count; i++) {
            if (mj_discard_value(c) != 0)
                return -1;
        }
        return 0;
    }
    case '{': {
        int64_t count;
        if (mj_long(c, &count) != 0)
            return -1;
        for (int64_t i = 0; i < count; i++) {
            if (mj_discard_value(c) != 0)
                return -1; /* key */
            if (mj_discard_value(c) != 0)
                return -1; /* value */
        }
        return 0;
    }
    default:
        return mj_fail(c);
    }
}

/* the memo'd span of a registered object, copied to the cursor. The
 * span lives earlier in the same buffer (append-only), so the copy
 * is memmove-safe. */
static int mj_emit_memo(mj_ctx* c, const mj_obj* o) {
    if (getenv("MJ_DEBUG")) {
        char prev[13];
        uint32_t k = o->out_len < 12 ? (uint32_t)o->out_len : 12;
        memcpy(prev, c->w->p + o->out_off, k);
        prev[k] = '\0';
        fprintf(stderr, "memo off=%zu len=%zu: %s\n", o->out_off, o->out_len, prev);
    }
    if (o->out_len == 0)
        return mj_fail(c); /* not memoized yet: a cycle */
    wr_reserve(c->w, o->out_len);
    if (c->w->oom)
        return -1;
    memmove(c->w->p + c->w->len, c->w->p + o->out_off, o->out_len);
    wr_col_update(c->w, c->w->p + c->w->len, (uint32_t)o->out_len);
    c->w->len += o->out_len;
    /* the raw advance bypasses wr_put: hand back the reservation it
     * would have consumed, or the next write rides past cap */
    c->w->reserve -= o->out_len;
    c->w->last = c->w->p[c->w->len - 1];
    return 0;
}

static int mj_value_inner(mj_ctx* c);

/* the wrapper owns the recursion budget: hostile bytes can nest arrays
 * without bound, the stack cannot (the parser's max_depth=1000 law) */
static int mj_value(mj_ctx* c) {
    if (c->depth >= MJ_DEPTH_MAX)
        return mj_fail(c);
    c->depth++;
    int rc = mj_value_inner(c);
    c->depth--;
    return rc;
}

static int mj_value_inner(mj_ctx* c) {
    unsigned char t;
    size_t tpos = c->s.at;
    int reg_idx = -1;
    size_t span_start = 0;
    int register_this = !c->no_reg;
    c->no_reg = 0;
    if (mj_byte(c, &t) != 0)
        return -1;
    /* arrays, hashes, strings, floats, bignums and anything
     * 'I'-wrapped register — ONCE per construct, at the outer byte
     * (the wrapper's no_reg suppresses the inner) (0-based stream
     * order). Bignums hold a slot but Ruby never emits a link to one
     * (the second occurrence re-emits in full); the entry only keeps
     * the slot count aligned for later links. Integers, symbols and
     * the other immediates never register. */
    if (register_this && (t == '[' || t == '{' || t == '"' || t == 'f' || t == 'I' || t == 'l')) {
        if (c->objs_n == c->objs_cap) {
            c->objs_cap = c->objs_cap ? c->objs_cap * 2 : 16;
            c->objs = (mj_obj*)realloc(c->objs, c->objs_cap * sizeof(*c->objs));
            if (!c->objs)
                return mj_fail(c);
        }
        c->objs[c->objs_n].pos = tpos;
        c->objs[c->objs_n].out_off = c->w->len;
        c->objs[c->objs_n].out_len = 0;
        reg_idx = (int)c->objs_n;
        c->objs_n++;
        span_start = c->w->len;
    }
    switch (t) {
    case '0':
        wr_put(c->w, "null", 4);
        break;
    case 'T':
        wr_put(c->w, "true", 4);
        break;
    case 'F':
        wr_put(c->w, "false", 5);
        break;
    case 'i': {
        int64_t v;
        if (mj_long(c, &v) != 0)
            return -1;
        char buf[24];
        int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
        wr_put(c->w, buf, (uint32_t)n);
        break;
    }
    case 'l': {
        /* the bignum: sign byte '+'/'-' + w_long(byte count) + LE
         * bytes; the i64-representable range emits, wider falls back */
        unsigned char sign;
        if (mj_byte(c, &sign) != 0)
            return -1;
        if (sign != 0x2b && sign != 0x2d)
            return mj_fail(c);
        int64_t nlimbs;
        if (mj_long(c, &nlimbs) != 0)
            return -1;
        if (nlimbs < 1 || nlimbs > 4)
            return mj_fail(c); /* beyond 64 bits: fall back */
        int64_t v = 0;
        for (int64_t i = 0; i < nlimbs; i++) {
            unsigned char lo, hi;
            if (mj_byte(c, &lo) != 0 || mj_byte(c, &hi) != 0)
                return -1;
            /* shift in the unsigned domain: the signed form put
             * 0xFFFF into the sign bit at limb 3 (UB, UBSan-flagged) */
            v |= (int64_t)((uint64_t)(lo | ((uint64_t)hi << 8)) << (16 * i));
        }
        if (sign == 0x2d)
            v = -v;
        char buf[24];
        int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
        wr_put(c->w, buf, (uint32_t)n);
        break;
    }
    case 'f': {
        int64_t len;
        if (mj_long(c, &len) != 0)
            return -1;
        if (len < 0 || (uint64_t)len > (uint64_t)(c->s.len - c->s.at))
            return mj_fail(c);
        char tmp[64];
        uint32_t tl = (uint32_t)len < sizeof tmp - 1 ? (uint32_t)len : (uint32_t)sizeof tmp - 1;
        memcpy(tmp, c->s.p + c->s.at, tl);
        tmp[tl] = '\0';
        c->s.at += (size_t)len;
        double d = strtod(tmp, NULL);
        if (!isfinite(d))
            return mj_fail(c); /* JSON has no inf/nan spelling — and a
                                * strtod OVERFLOW ("1e999") produces
                                * one from text that never spells it */
        char buf[40];
        int wl = yep_d2s_json(d, buf);
        wr_put(c->w, buf, (uint32_t)wl);
        break;
    }
    case '"': {
        int64_t len;
        if (mj_long(c, &len) != 0)
            return -1;
        int rc = mj_string_body(c, len);
        if (rc != 0)
            return -1;
        break;
    }
    case 'I': {
        /* the wrapped value IS the payload: re-dispatch into it, then
         * the ivar pairs ride. The inner dispatch must not register —
         * the construct registered at the 'I' (the double entry
         * shifted every later '@' link) */
        c->no_reg = 1;
        if (mj_value(c) != 0) {
            c->no_reg = 0;
            return -1;
        }
        c->no_reg = 0;
        if (mj_ivar_pairs(c) != 0)
            return -1;
        break;
    }
    case ':': {
        /* a bare symbol VALUE: emitted as its name string */
        if (mj_symbol(c, 1) != 0)
            return -1;
        break;
    }
    case ';': {
        int64_t idx;
        if (mj_long(c, &idx) != 0)
            return -1;
        if (idx < 0 || (size_t)idx >= c->syms_n)
            return mj_fail(c);
        emit_dq_json(c->w, c->syms[idx].p, c->syms[idx].len);
        break;
    }
    case '@': {
        /* an object link: the registered span memmoves to the cursor */
        int64_t idx;
        if (mj_long(c, &idx) != 0)
            return -1;
        if (idx < 0 || (size_t)idx >= c->objs_n)
            return mj_fail(c);
        if (mj_emit_memo(c, &c->objs[idx]) != 0)
            return -1;
        break;
    }
    case '[':
    case '{': {
        int64_t count;
        if (mj_long(c, &count) != 0)
            return -1;
        if (count < 0)
            return mj_fail(c);
        wr_byte(c->w, t == '[' ? '[' : '{');
        const char* sep = c->compact ? "," : ", ";
        uint32_t sep_len = c->compact ? 1u : 2u;
        for (int64_t i = 0; i < count; i++) {
            if (i > 0)
                wr_put(c->w, sep, sep_len);
            if (t == '[') {
                if (mj_value(c) != 0)
                    return -1;
            } else {
                /* the key: a string (with its 'I' wrapper) or a
                 * symbol — JSON keys are strings */
                unsigned char kt;
                size_t ktpos = c->s.at;
                if (mj_byte(c, &kt) != 0)
                    return -1;
                if (kt == 'I') {
                    if (mj_value(c) != 0)
                        return -1;
                    if (mj_ivar_pairs(c) != 0)
                        return -1;
                } else if (kt == '"') {
                    /* the key registers like any string object: its
                     * span is memo'd for later '@' key links */
                    int kreg = (int)c->objs_n;
                    size_t kspan = c->w->len;
                    if (kreg >= 0) {
                        if (c->objs_n == c->objs_cap) {
                            c->objs_cap = c->objs_cap ? c->objs_cap * 2 : 16;
                            c->objs = (mj_obj*)realloc(c->objs, c->objs_cap * sizeof(*c->objs));
                            if (!c->objs)
                                return mj_fail(c);
                        }
                        c->objs[c->objs_n].pos = ktpos;
                        c->objs[c->objs_n].out_off = c->w->len;
                        c->objs[c->objs_n].out_len = 0;
                        c->objs_n++;
                    }
                    int64_t len;
                    if (mj_long(c, &len) != 0)
                        return -1;
                    int rc = mj_string_body(c, len);
                    if (rc != 0)
                        return -1;
                    if (kreg >= 0) {
                        c->objs[kreg].out_off = kspan;
                        c->objs[kreg].out_len = c->w->len - kspan;
                    }
                } else if (kt == ':') {
                    if (mj_symbol(c, 1) != 0)
                        return -1;
                } else if (kt == ';') {
                    int64_t idx;
                    if (mj_long(c, &idx) != 0)
                        return -1;
                    if (idx < 0 || (size_t)idx >= c->syms_n)
                        return mj_fail(c);
                    emit_dq_json(c->w, c->syms[idx].p, c->syms[idx].len);
                } else if (kt == '@') {
                    int64_t idx;
                    if (mj_long(c, &idx) != 0)
                        return -1;
                    if (idx < 0 || (size_t)idx >= c->objs_n)
                        return mj_fail(c);
                    if (mj_emit_memo(c, &c->objs[idx]) != 0)
                        return -1;
                } else {
                    return mj_fail(c);
                }
                wr_put(c->w, c->compact ? ":" : ": ", c->compact ? 1u : 2u);
                if (mj_value(c) != 0)
                    return -1;
            }
        }
        wr_byte(c->w, t == '[' ? ']' : '}');
        break;
    }
    default:
        return mj_fail(c);
    }
    if (reg_idx >= 0) {
        c->objs[reg_idx].out_off = span_start;
        c->objs[reg_idx].out_len = c->w->len - span_start;
    }
    return 0;
}

YEPTRIS_API YeptrisStatus yeptris_json_from_marshal(const char* m, size_t m_len, int compact,
                                                    char** out, size_t* out_len) {
    if (out == NULL || out_len == NULL)
        return YEPTRIS_ERROR_ARG;
    *out = NULL;
    *out_len = 0;
    if (m == NULL || m_len < 2 || (unsigned char)m[0] != 0x04 || (unsigned char)m[1] != 0x08)
        return YEPTRIS_ERROR_UNSUPPORTED;
    yep_emitter em;
    memset(&em, 0, sizeof(em));
    yep_writer* w = &em.w;
    w->sc_dec = NULL;
    w->sc_i = 0;
    w->json = 1;
    w->json_compact = compact ? 1 : 0;
    w->json_pretty = 0;
    w->canonical = 0;
    w->explicit_doc_start = 0;
    w->best_width = 0;
    w->sv_input = NULL;
    w->sv_arena = NULL;
    w->sink = NULL;
    w->force_flow = 0;
    w->in_flow = 0;
    w->flow_item = 0;
    w->unserializable = 0;

    mj_ctx c;
    memset(&c, 0, sizeof(c));
    c.w = w;
    c.s.p = m;
    c.s.len = m_len;
    c.s.at = 2; /* the 4.8 header */
    c.compact = compact ? 1 : 0;

    /* one wet pass into a growable buffer (the #352 slice-2 shape) */
    size_t est = m_len * 2 + 64;
    char* buf = (char*)malloc(est);
    if (!buf)
        return YEPTRIS_ERROR_MEMORY;
    w->p = buf;
    w->cap = est;
    w->grow = 1;

    mj_value(&c);

    int failed = c.failed;
    size_t wrote = w->len;
    buf = w->p;
    free(c.syms);
    free(c.objs);
    if (failed || w->oom) {
        free(buf);
        return failed ? (YeptrisStatus)failed : YEPTRIS_ERROR_MEMORY;
    }
    buf[wrote] = '\0';
    *out = buf;
    *out_len = wrote;
    return YEPTRIS_OK;
}

size_t yep_emit_run(yep_emitter* em, int dry) {
    yep_writer* w = &em->w;
    const yep_dom* d = em->doc->dom;
    w->dry = dry;
    w->len = 0;
    w->reserve = 0;
    w->last = 0;
    w->force_flow = 0;
    w->flushed = 0;
    w->sink_aborted = 0;
    w->col = 0;
    w->sc_i = 0;
    yep_nametab_clear(&em->canon_names);
    if (w->json) {
        /* JSON has no multi-document streams and no document marker */
        if (d->dcount == 1) {
            const yep_dnode* root = yep_dom_node(d, d->docs[0]);
            if (w->json_pretty) {
                emit_node(em, d->docs[0], -2, 0);
            } else {
                emit_json_node(em, d->docs[0]);
            }
            if (root != NULL && root->kind != 0 && w->last != '\n') {
                wr_byte(w, '\n');
            }
        }
        return w->flushed + w->len;
    }
    int multi = (d->dcount > 1) || w->canonical;
    for (uint32_t i = 0; i < d->dcount; i++) {
        const yep_dnode* root = yep_dom_node(d, d->docs[i]);
        if (multi || w->explicit_doc_start) {
            /* libyaml parity: a scalar root rides the marker line
             * ("--- 42"); collections take the marker alone */
            wr_put(w,
                   (w->explicit_doc_start && root != NULL && root->kind == YEP_DOM_SCALAR &&
                    !root->flow)
                       ? "--- "
                       : "---\n",
                   4);
        }
        if (root != NULL && (root->kind == 1 || root->kind == 2) && !root->flow && !w->canonical &&
            root->count > 0 && (root->tag.len > 0 || root->anchor.len > 0)) {
            /* count > 0: an EMPTY propertied collection takes
             * emit_node's inline empty-form ("! &a {}") — the leading
             * props line DUPLICATED it ("! &a\n! &a {}") and every
             * round shed a line (the nightly's run 38167237445; the
             * catch-56 law, root side) */
            /* root block-collection props ride their own leading line;
             * scalars and flow collections carry them inline */
            if (root->tag.len > 0) {
                yep_view tdec3 = wv(w, root->tag);
                wr_tag_token(w, &tdec3);
            }
            if (root->anchor.len > 0) {
                if (root->tag.len > 0) {
                    wr_byte(w, ' '); /* props need the separator */
                }
                yep_view adec3 = wv(w, root->anchor);
                wr_byte(w, '&');
                wr_put(w, (const char*)adec3.p, adec3.len);
            }
            wr_byte(w, '\n');
        }
        emit_node(em, d->docs[i], -2, 0); /* root content at column 0 */
        if (w->last != '\n') {
            wr_byte(w, '\n');
        }
    }
    return w->flushed + w->len;
}

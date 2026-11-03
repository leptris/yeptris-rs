/* scan.c — line facts and span location (TODO.impl/06).
 *
 * Byte classification truth lives in chartype; SIMD kernels accelerate
 * the span scans; this module owns YAML's line/span semantics only.
 */

#include <string.h>

#include "common/chartype.h"
#include "common/simd_text.h"
#include "scan.h"

size_t yep_scan_break_len(const char* p, size_t len, size_t pos) {
    if (pos >= len) {
        return 0;
    }
    if (p[pos] == '\n') {
        return 1;
    }
    if (p[pos] == '\r') {
        return (pos + 1 < len && p[pos + 1] == '\n') ? 2 : 1;
    }
    /* NEL/LS/PS are breaks like LF/CR (libyaml's IS_BREAK): the byte
     * scanners ride them as content otherwise, and a plain value
     * folds across the break INTO a following document marker (the
     * nightly's r74: "&k:\":(<PS>---" read as one scalar, libyaml
     * two events) */
    return yep_scan_mb_break_len(p, len, pos);
}

size_t yep_scan_mb_break_len(const char* p, size_t len, size_t pos) {
    if (pos + 1 < len && (unsigned char)p[pos] == 0xC2 && (unsigned char)p[pos + 1] == 0x85) {
        return 2; /* NEL */
    }
    if (pos + 2 < len && (unsigned char)p[pos] == 0xE2 && (unsigned char)p[pos + 1] == 0x80 &&
        ((unsigned char)p[pos + 2] & 0xFE) == 0xA8) {
        return 3; /* LS / PS */
    }
    return 0;
}

void yep_scan_advance_line(const char* p, size_t* from, size_t to, uint32_t* line,
                           size_t* line_start) {
    for (size_t i = *from; i < to; i++) {
        if (p[i] == '\n') {
            (*line)++;
            *line_start = i + 1;
        } else if (p[i] == '\r' && i + 1 < to && p[i + 1] != '\n') {
            /* a lone \r is a break too (the engine's break rule):
             * the JSON fast path's walk missed it and marked every
             * event after the \r one line low while the general
             * kernel (which walks \r) marked them correctly — whole
             * vs chunked diverged on `\r`-containing JSON-class flow
             * (the nightly's run 38549939326) */
            (*line)++;
            *line_start = i + 1;
        } else {
            size_t mb = yep_scan_mb_break_len(p, to, i);
            if (mb > 0) {
                (*line)++;
                i += mb - 1;
                *line_start = i + 1;
            }
        }
    }
    *from = to;
}

/* ONE home for the \n/\r break class (scan owns the concept): the
 * engine's multiline walk shares it — it had rebuilt a duplicate per
 * quoted scalar. Prebuilt nibble-class tables (TODO.restructure/68);
 * the StopsetLiterals spec pins them against yep_stopset_init. */
const yep_stopset yep_break_stopset = {
    .bitmap = {0x00, 0x24, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    .groups = 1,
    .lo = {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02,
            0x00, 0x00}},
    .hi = {{0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00}},
};

void yep_scan_facts(const char* p, size_t len, size_t pos, yep_line_facts* out) {
    yep_text_active()->line_facts(p, len, pos, out);
}

/* Line facts -> line info (the flags/marker derivation, facts-free).
 * One kernel pass feeds BOTH this and the shape classifier via the
 * engine's combined memo (TODO.restructure/76). */
void yep_scan_line_f(const char* p, size_t len, size_t pos, const yep_line_facts* f,
                     yep_line_info* out) {
    yep_line_info li;
    li.offset = (uint32_t)pos;
    li.indent = 0;
    li.flags = 0;
    li.first = 0;
    li.end = f->end;
    size_t j = f->indent;
    li.indent = (uint32_t)(j - pos);
    if (j < li.end && p[j] == '\t') {
        size_t k = j;
        while (k < li.end && (p[k] == ' ' || p[k] == '\t')) {
            k++;
        }
        if (k >= li.end) {
            li.flags |= YEP_LF_BLANK;
            if (li.indent == 0) {
                li.flags |= YEP_LF_TAB;
            }
            *out = li;
            return;
        }
        li.flags |= YEP_LF_TAB;
    }

    if (j >= li.end) {
        li.flags |= YEP_LF_BLANK;
        *out = li;
        return;
    }

    li.first = (unsigned char)p[j];
    if (li.first == '#') {
        li.flags |= YEP_LF_COMMENT;
        *out = li;
        return;
    }
    if (li.first == '%' && li.indent == 0) {
        li.flags |= YEP_LF_DIRECTIVE;
        *out = li;
        return;
    }

    /* "---" / "..." at column 0, followed by EOL/space/tab/comment —
     * content may follow on the same line ("--- > folded"). */
    if (li.indent == 0 && li.end - j >= 3 && memcmp(p + j, "---", 3) == 0 &&
        (li.end - j == 3 || p[j + 3] == ' ' || p[j + 3] == '\t')) {
        li.flags |= YEP_LF_DOC_START;
    } else if (li.indent == 0 && li.end - j >= 3 && memcmp(p + j, "...", 3) == 0 &&
               (li.end - j == 3 || p[j + 3] == ' ' || p[j + 3] == '\t')) {
        li.flags |= YEP_LF_DOC_END;
    }
    *out = li;
}

yep_line_info yep_scan_line(const char* p, size_t len, size_t pos) {
    yep_line_info li;
    yep_line_facts f;
    yep_scan_facts(p, len, pos, &f);
    yep_scan_line_f(p, len, pos, &f, &li);
    return li;
}

/* Terminator check for ':' — blank, EOL, or (in flow) a flow indicator. */
static int yep_colon_terminates(const char* p, size_t len, size_t colon, int flow) {
    size_t next = colon + 1;
    if (next >= len) {
        return 1; /* ':' at EOF terminates */
    }
    unsigned char c = (unsigned char)p[next];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        return 1;
    }
    /* a raw NEL/LS/PS is a break like LF (libyaml's IS_BLANKZ): the
     * value-position separator arm the #833 dig pinned — the LF twin
     * terminated while the PS spelling kept the colon in the plain */
    if (yep_scan_mb_break_len(p, len, next) > 0) {
        return 1;
    }
    if (flow && yep_ct_is(c, YEP_CT_FLOW_IND)) {
        return 1;
    }
    return 0;
}

int yep_plain_first_ok(unsigned char c) {
    return !(c == ',' || c == ']' || c == '}' || c == '%' || c == '@' || c == '`');
}

int yep_scan_prop_char(unsigned char c) {
    return !yep_ct_any(c, YEP_CT_BLANK | YEP_CT_LBREAK | YEP_CT_FLOW_IND) && c != ',' && c != '#';
}

int yep_scan_dq_forced(const char* p, uint32_t len) {
    for (uint32_t k = 0; k + 1 < len; k++) {
        if ((unsigned char)p[k] == 0xc2 && (unsigned char)p[k + 1] == 0x85) {
            return 1;
        }
        /* LS/PS are breaks like NEL: only their escapes are stable */
        if ((unsigned char)p[k] == 0xe2 && k + 2 < len && (unsigned char)p[k + 1] == 0x80 &&
            ((unsigned char)p[k + 2] == 0xa8 || (unsigned char)p[k + 2] == 0xa9)) {
            return 1;
        }
        if ((unsigned char)p[k] == 0xef && k + 2 < len && (unsigned char)p[k + 1] == 0xbb &&
            (unsigned char)p[k + 2] == 0xbf) {
            return 1;
        }
        if ((unsigned char)p[k] == 0xef && k + 2 < len && (unsigned char)p[k + 1] == 0xbf &&
            ((unsigned char)p[k + 2] == 0xbe || (unsigned char)p[k + 2] == 0xbf)) {
            return 1; /* noncharacter: only its escape re-parses */
        }
    }
    return 0;
}

/* ---- the plain-safety truth table (moved from emit/style.c,
 * #352 lane 3: scan owns bytes→facts; the emitter and the DOM's
 * parse-time fact bit both read THIS walk so they cannot drift) ---- */

static int is_blank(char c) {
    return c == ' ' || c == '\t';
}

static int is_break(char c) {
    return c == '\n' || c == '\r';
}

int yep_scan_plain_safe(const char* p, uint32_t len) {
    if (len == 0) {
        return 0; /* empty needs quotes or an empty-value slot */
    }
    if (is_blank(p[0]) || is_blank(p[len - 1])) {
        return 0;
    }
    if (len >= 3 && (unsigned char)p[0] == 0xef && (unsigned char)p[1] == 0xbb &&
        (unsigned char)p[2] == 0xbf) {
        /* a LEADING U+FEFF re-parses as a document BOM and vanishes —
         * the emitted form must carry its escape ("\uFEFF", the
         * nightly's run 38258674337); mid-token BOMs ride plain */
        return 0;
    }
    switch (p[0]) {
    case ':':
        /* libyaml (Psych symbol dumps ":name"): a leading colon only
         * indicates when followed by a blank/EOL — the interior rule
         * below covers it; ":name" stays plain */
        break;
    case '-':
    case '?':
        /* indicators only before a blank or EOL: "-5" and "?x" are
         * plain (libyaml emits negative numbers plain; Psych, too) —
         * but fall through: the document-marker check below owns
         * the bare "---"/"..." (libyaml quotes those) */
        if (len == 1 || is_blank(p[1])) {
            return 0;
        }
        break;
    case ',':
    case '[':
    case ']':
    case '{':
    case '}':
    case '#':
    case '&':
    case '*':
    case '!':
    case '|':
    case '>':
    case '\'':
    case '"':
    case '%':
    case '@':
    case '`':
        return 0;
    default:
        break;
    }
    /* The interior scan in SWAR words: every reject condition is a
     * byte-class adjacency, so one masked word test is exact —
     * break/tab bytes reject directly; ':' rejects with a blank at its
     * NEXT lane (or the span's EOL); '#' with a blank at its PREVIOUS
     * lane (the span's first byte is exempt — the head switch already
     * handled a leading '#'). Cross-word adjacency rides one carried
     * byte per boundary; the tail runs the byte loop, whose rules read
     * real neighbors and need no carries. */
    {
        const uint64_t ONES = 0x0101010101010101ull;
        const uint64_t HIGH = 0x8080808080808080ull;
#define YST_ZDET(w, ch)                                                                            \
    ((((w) ^ (uint64_t)(ch) * ONES) - ONES) & ~((w) ^ (uint64_t)(ch) * ONES) & HIGH)
        /* the all-boring accept (#742): a span of pure [0-9A-Za-z_]
         * makes EVERY rule in this function vacuous — no indicators
         * (the head switch), no blanks at the ends, no BOM, no
         * ': '/' #' adjacency, no breaks, and no ---/... marker — so
         * one range test per word accepts without the six-mask walk.
         * Anything else falls through to the exact scan below. */
        {
            uint32_t bi = 0;
            int boring = 1;
            while (bi + 8 <= len) {
                uint64_t bw;
                memcpy(&bw, p + bi, 8);
                if (bw & HIGH) {
                    boring = 0;
                    break;
                }
                /* per-lane unsigned range checks via borrow bits: alpha
                 * is (t - 'a') and ('z' - t) both borrow-free with
                 * t = w|0x20 (A-Z fold); digits bound 0x30..0x39 */
                uint64_t t = bw | (ONES * 0x20);
                uint64_t bad = ((t - ONES * 0x61) | (ONES * 0x7A - t)) & HIGH; /* alpha */
                bad |= ((bw - ONES * 0x30) | (ONES * 0x39 - bw)) & HIGH;       /* digit */
                bad &= ~YST_ZDET(bw, '_'); /* the one extra boring byte */
                if (bad != 0) {
                    boring = 0;
                    break;
                }
                bi += 8;
            }
            if (boring) {
                for (; bi < len; bi++) {
                    char c = p[bi];
                    int a = (c | 0x20);
                    if (!((c >= '0' && c <= '9') || (a >= 'a' && a <= 'z') || c == '_')) {
                        boring = 0;
                        break;
                    }
                }
                if (boring) {
                    return 1;
                }
            }
        }
        uint32_t i = 0;
        int prev_blank7 = 0;
        while (i + 8 <= len) {
            uint64_t w;
            memcpy(&w, p + i, 8);
            if (w & HIGH) {
                /* multibyte content: the byte loop owns the rest — a raw
                 * LS/PS/NEL rides in a scalar value since the quote-aware
                 * normalizer (#638) and is a line break on re-parse, so
                 * the value must be quoted, not plain (the nightly's
                 * round 18, crash-1e79b2e3) */
                break;
            }
            uint64_t space = YST_ZDET(w, ' ');
            uint64_t tab = YST_ZDET(w, '\t');
            uint64_t blank = space | tab;
            uint64_t colon = YST_ZDET(w, ':');
            uint64_t hash = YST_ZDET(w, '#');
            /* tab and the breaks reject outright */
            if (tab | YST_ZDET(w, '\n') | YST_ZDET(w, '\r')) {
                return 0;
            }
            /* ':' + blank next: (blank >> 8) pulls lane i+1's flag down
             * into lane i; the word's last lane takes the peeked byte
             * (EOL at the span end counts as blank — the rule rejects
             * there) */
            if (colon) {
                int next_blank = i + 8 < len ? is_blank(p[i + 8]) : 1;
                if (colon & ((blank >> 8) | (next_blank ? (uint64_t)0x80 << 56 : 0))) {
                    return 0;
                }
            }
            /* '#' + blank prev: (blank << 8) lifts lane i-1's flag up
             * into lane i; lane 0 takes the carried last-lane flag */
            if (hash && (hash & ((blank << 8) | (prev_blank7 ? 0x80 : 0)))) {
                return 0;
            }
            prev_blank7 = (int)((blank >> 63) & 1); /* lane 7's flag is its top bit */
            i += 8;
        }
#undef YST_ZDET
        for (; i < len; i++) {
            char c = p[i];
            if (is_break(c) || c == '\t') {
                return 0;
            }
            if ((unsigned char)c == 0xE2 && i + 2 < len && (unsigned char)p[i + 1] == 0x80 &&
                ((unsigned char)p[i + 2] & 0xFE) == 0xA8) {
                return 0; /* a raw LS/PS re-parses as a line break */
            }
            if ((unsigned char)c == 0xC2 && i + 1 < len && (unsigned char)p[i + 1] == 0x85) {
                return 0; /* a raw NEL ditto */
            }
            if (c == ':') {
                if (i + 1 >= len || is_blank(p[i + 1])) {
                    return 0;
                }
            }
            if (c == '#') {
                if (i > 0 && is_blank(p[i - 1])) {
                    return 0;
                }
            }
        }
    }
    /* document markers */
    if (len >= 3 && (memcmp(p, "---", 3) == 0 || memcmp(p, "...", 3) == 0) &&
        (len == 3 || p[3] == ' ' || p[3] == '\t' || p[3] == '\n' || p[3] == '\r')) {
        /* a PREFIX marker is worse than the bare one: emitted plain at
         * line start, "--- ..." re-reads as a document marker (the
         * nightly's "--- --- >" folded-plain crasher) — libyaml quotes
         * both */
        return 0;
    }
    return 1;
}

size_t yep_scan_prop_end(const char* p, size_t len, size_t pos) {
    while (pos < len) {
        unsigned char c = (unsigned char)p[pos];
        if (!yep_scan_prop_char(c)) {
            break;
        }
        /* NEL/LS/PS end the name (libyaml's IS_BLANKZ anchor law: the
         * suite's ns-anchor-char excludes breaks): the byte-level
         * table cannot classify a multibyte break, and the name run
         * then swallowed it — the r69/r70 nightly divergence class
         * ("&k:":(<PS>..."), shape-dependent clip downstream */
        if (c == 0xC2 && pos + 1 < len && (unsigned char)p[pos + 1] == 0x85) {
            break;
        }
        if (c == 0xE2 && pos + 2 < len && (unsigned char)p[pos + 1] == 0x80 &&
            ((unsigned char)p[pos + 2] & 0xFE) == 0xA8) {
            break;
        }
        pos++;
    }
    return pos;
}

int yep_scan_tag_char(unsigned char c) {
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
        return 1;
    }
    if (c >= 0x80) {
        return 1; /* UTF-8 suffix bytes pass through */
    }
    switch (c) {
    case '-':
    case '_':
    case ';':
    case '/':
    case '?':
    case ':':
    case '@':
    case '&':
    case '=':
    case '+':
    case '$':
    case '.':
    case '!':
    case '~':
    case '*':
    case '\'':
    case '(':
    case ')':
    case '%':
        return 1;
    default:
        return 0;
    }
}

/* ---- line-shape classification (TODO.restructure/49) ---- */

/* blank/EOL/EOF directly after p[at] (the dash / explicit-key rule). */
static int shape_blank_next(const char* p, size_t len, size_t at) {
    return at + 1 >= len || p[at + 1] == ' ' || p[at + 1] == '\t' || p[at + 1] == '\n' ||
           p[at + 1] == '\r';
}

/* Value bytes the fast arms do not own: quoted/tagged/block/flow
 * scalars and the compact-syntax leads whose semantics belong to the
 * general chain. */
static int shape_val_bail(unsigned char c, const char* p, size_t len, size_t at) {
    switch (c) {
    case '"':
    case '\'':
    case '!':
    case '|':
    case '>':
    case '[':
    case '{':
    case '*': /* an alias after an anchor is an error shape (SR86) */
    case '&': /* a second anchor is the suite's 4JVG error (catch 156) */
        return 1;
    case '-':
    case '?':
        return shape_blank_next(p, len, at);
    default:
        return 0;
    }
}

static void shape_value(const char* p, size_t len, const yep_line_info* li, size_t vt,
                        yep_line_shape* s) {
    s->val_start = (uint32_t)vt;
    if (vt >= li->end || p[vt] == '#') { /* '#' here follows a blank by construction */
        s->val = YEP_LVAL_EMPTY;
        return;
    }
    unsigned char c = (unsigned char)p[vt];
    if (c == '*') {
        s->val = YEP_LVAL_ALIAS; /* the engine's alias walk owns name semantics */
        return;
    }
    if (c == '[' || c == '{') {
        s->val = YEP_LVAL_FLOW;
        return;
    }
    if (c == '&') {
        size_t a_end = yep_scan_prop_end(p, len, vt + 1);
        if (a_end == vt + 1) {
            return; /* an anchor needs a name (libyaml): the general
                     * chain owns the error */
        }
        size_t t = a_end;
        while (t < len && (p[t] == ' ' || p[t] == '\t')) {
            t++;
        }
        if (t < li->end && p[t] != '#' && !shape_val_bail((unsigned char)p[t], p, len, t) &&
            yep_plain_first_ok((unsigned char)p[t])) {
            yep_span v = yep_scan_plain(p, len, t, 0);
            if (v.term != YEP_TERM_COLON && v.end > v.start) {
                s->val = YEP_LVAL_ANCHOR_PLAIN;
                s->anchor_end = (uint32_t)a_end;
                s->val_span = v;
                return;
            }
        }
        return; /* anchor with a following-lines value / odd content: bail */
    }
    if (shape_val_bail(c, p, len, vt) || !yep_plain_first_ok(c)) {
        return;
    }
    yep_span v = yep_scan_plain(p, len, vt, 0);
    if (v.term == YEP_TERM_COLON) {
        return; /* a second terminating ':' is compact/error territory */
    }
    s->val = YEP_LVAL_PLAIN;
    s->val_span = v;
}

/* The key span straight from the fused facts (TODO.restructure/76):
 * the common line's key scan becomes zero byte-walking. Rare shapes
 * (interior ':', embedded '#') fall back to the walking scan — the
 * two must agree, and the LineShape spec pins them together. */
static yep_span shape_key_span(const char* p, size_t len, size_t t, const yep_line_facts* f) {
    yep_span k;
    k.start = (uint32_t)t;
    k.term = YEP_TERM_EOF;
    if (f->stop_set && f->stop >= t) {
        unsigned char c = (unsigned char)p[f->stop];
        if (c == ':') {
            if (yep_colon_terminates(p, len, f->stop, 0)) {
                k.term = YEP_TERM_COLON;
                k.end = f->stop;
                while (k.end > k.start && (p[k.end - 1] == ' ' || p[k.end - 1] == '\t')) {
                    k.end--;
                }
                return k;
            }
        } else if (c == '#' && (f->stop == t || p[f->stop - 1] == ' ' || p[f->stop - 1] == '\t')) {
            k.term = YEP_TERM_COMMENT;
            k.end = f->stop;
            while (k.end > k.start && (p[k.end - 1] == ' ' || p[k.end - 1] == '\t')) {
                k.end--;
            }
            return k;
        }
        return yep_scan_plain(p, len, t, 0); /* non-terminating stop: the walk owns it */
    }
    /* no stop byte before EOL: the span runs to the line end */
    k.end = f->end;
    while (k.end > k.start && (p[k.end - 1] == ' ' || p[k.end - 1] == '\t')) {
        k.end--;
    }
    k.term = f->end < len ? YEP_TERM_EOL : YEP_TERM_EOF;
    return k;
}

void yep_scan_shape_f(const char* p, size_t len, const yep_line_info* li, const yep_line_facts* f,
                      yep_line_shape* s) {
    memset(s, 0, sizeof(*s));
    s->kind = YEP_LSHAPE_NONE;
    s->val = YEP_LVAL_NONE;
    if (li->flags != 0) {
        return; /* blank/comment/doc/directive/tab lines: general paths */
    }
    size_t t = li->offset + li->indent;
    unsigned char c = (unsigned char)p[t];

    if (c == '-' && shape_blank_next(p, len, t)) {
        size_t vt = t + 1;
        while (vt < len && (p[vt] == ' ' || p[vt] == '\t')) {
            vt++;
        }
        s->kind = YEP_LSHAPE_DASH;
        s->dash = (uint32_t)t;
        shape_value(p, len, li, vt, s);
        return;
    }

    /* '?' / ':' followed by a blank are the explicit-key and bare-colon
     * lines (the general paths own them); followed by content they are
     * ordinary plain-first bytes. A dash + blank returned as DASH above. */
    if (yep_plain_first_ok(c) && c != '&' && c != '!' && c != '*' && c != '\'' && c != '"' &&
        c != '[' && c != '{' && !((c == '?' || c == ':') && shape_blank_next(p, len, t))) {
        yep_span k = shape_key_span(p, len, t, f);
        if (k.term == YEP_TERM_COLON) {
            /* k.end is the TRIMMED span end: the ':' sits past the
             * trimmed blanks (or past interior stops on the fallback
             * path) — the forward walk is over spaces, a few bytes */
            size_t colon = k.end;
            while (p[colon] != ':') {
                colon++;
            }
            size_t vt = colon + 1;
            while (vt < len && (p[vt] == ' ' || p[vt] == '\t')) {
                vt++;
            }
            s->kind = YEP_LSHAPE_KEY;
            s->key_start = k.start;
            s->key_end = k.end;
            s->colon = (uint32_t)colon;
            shape_value(p, len, li, vt, s);
        }
    }
}

void yep_scan_shape(const char* p, size_t len, const yep_line_info* li, yep_line_shape* s) {
    yep_line_facts f;
    yep_scan_facts(p, len, li->offset, &f);
    yep_scan_shape_f(p, len, li, &f, s);
}

/* The two plain-scalar stop classes are constants — they were rebuilt
   on EVERY scan (~2 per line; clear + 4-9 adds ≈ 3M ops on a 100k-line
   document), then became static bitmaps, now prebuilt nibble-class
   tables (TODO.restructure/68). Generated: set[c>>3] |= 1 << (c&7) for
   the stop chars below; lo/hi pinned by the StopsetLiterals spec. */
const yep_stopset yep_plain_stop_block = {
    .bitmap = {0x00, 0x24, 0x00, 0x00, 0x08, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    /* members: \n \r # : */
    .groups = 1,
    .lo = {{0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x02,
            0x00, 0x00}},
    .hi = {{0x03, 0x00, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00}},
};

const yep_stopset yep_plain_stop_flow = {
    .bitmap = {0x00, 0x24, 0x00, 0x00, 0x08, 0x10, 0x00, 0x04, 0x00, 0x00, 0x00,
               0x28, 0x00, 0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    /* members: \n \r # , : [ ] { } */
    .groups = 2,
    .lo = {{0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x11, 0xA0, 0x08, 0x42,
            0x00, 0x00},
           {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
            0x00, 0x00}},
    .hi = {{0x03, 0x00, 0x0C, 0x10, 0x00, 0x60, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00},
           {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00}},
};

yep_span yep_scan_plain(const char* p, size_t len, size_t pos, int flow) {
    const yep_text_kernels* k = yep_text_active();
    const yep_stopset* stop = flow ? &yep_plain_stop_flow : &yep_plain_stop_block;
    /* Short spans keep the scalar walk: the kernel dispatch costs more
     * than it saves below a vector (block keys run 2-8 bytes — the
     * same gate scan_line uses for its end-find). */
    const int tiny = (len - pos) < 64;

    yep_span s;
    s.start = (uint32_t)pos;
    s.end = (uint32_t)pos;
    s.term = YEP_TERM_EOF;

    size_t i = pos;
    while (i < len) {
        ptrdiff_t hit;
        if (tiny) {
            size_t at = i;
            while (at < len && !yep_stopset_test(stop->bitmap, (unsigned char)p[at])) {
                at++;
            }
            hit = (at < len) ? (ptrdiff_t)(at - i) : -1;
        } else {
            hit = k->stopset_find(stop, p + i, len - i);
        }
        size_t at = (hit < 0) ? len : i + (size_t)hit;
        /* a multibyte break inside the piece ends it like an EOL: the
         * byte stopset cannot see NEL/LS/PS, and the continuation
         * would ride across the break into the next line's tokens */
        if (at > i) {
            const char* lead = (const char*)memchr(p + i, 0xC2, at - i);
            const char* lead2 = (const char*)memchr(p + i, 0xE2, at - i);
            const char* first =
                (lead && lead2) ? (lead < lead2 ? lead : lead2) : (lead ? lead : lead2);
            if (first != NULL && yep_scan_mb_break_len(p, len, (size_t)(first - p)) > 0) {
                at = (size_t)(first - p);
                i = at;
                s.term = YEP_TERM_EOL;
                break;
            }
        }
        unsigned char c = (at < len) ? (unsigned char)p[at] : 0;

        if (at == len) {
            i = len;
            s.term = YEP_TERM_EOF;
            break;
        }
        if (c == '\n' || c == '\r') {
            i = at;
            s.term = YEP_TERM_EOL;
            break;
        }
        if (c == ':') {
            if (yep_colon_terminates(p, len, at, flow)) {
                i = at;
                s.term = YEP_TERM_COLON;
                break;
            }
            i = at + 1;
            continue;
        }
        if (c == '#') {
            /* '#' starts a comment after a blank or at span start */
            if (at == s.start || (at > s.start && (p[at - 1] == ' ' || p[at - 1] == '\t'))) {
                i = at;
                s.term = YEP_TERM_COMMENT;
                break;
            }
            i = at + 1;
            continue;
        }
        /* flow indicator (flow context only) */
        i = at;
        s.term = YEP_TERM_FLOW;
        break;
    }

    /* Trim trailing spaces/tabs from the span. */
    size_t e = i;
    while (e > s.start && (p[e - 1] == ' ' || p[e - 1] == '\t')) {
        e--;
    }
    s.end = (uint32_t)e;
    if (e == s.start && s.term == YEP_TERM_EOF) {
        s.term = YEP_TERM_EOF;
    }
    return s;
}

yep_span yep_scan_quoted(const char* p, size_t len, size_t pos, int* has_escape) {
    const yep_text_kernels* k = yep_text_active();
    unsigned char q = (unsigned char)p[pos];
    int esc = 0;
    ptrdiff_t r = k->quote_scan(p + pos + 1, len - pos - 1, (char)q, &esc);
    if (has_escape != NULL) {
        *has_escape = esc;
    }
    yep_span s;
    s.start = (uint32_t)(pos + 1);
    if (r < 0) {
        s.end = (uint32_t)len;
        s.term = YEP_TERM_EOL; /* unterminated — caller reports the error */
    } else {
        s.end = (uint32_t)(pos + 1 + (size_t)r);
        s.term = YEP_TERM_EOF;
    }
    return s;
}

int yep_scan_is_key_start(unsigned char c) {
    /* Quotes and flow openers always start a potential key; plain-first
     * excludes most indicators. */
    if (c == '\'' || c == '"' || c == '[' || c == '{' || c == '?') {
        return 1;
    }
    if (c == '&' || c == '!' || c == '*') {
        return 1; /* properties or an alias may open a key node */
    }
    if (yep_ct_is(c, YEP_CT_INDICATOR)) {
        return 0; /* the remaining indicators cannot start a plain scalar */
    }
    return yep_ct_is_ns(c);
}

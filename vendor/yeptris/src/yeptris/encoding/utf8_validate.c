/* utf8_validate.c — well-formedness check with a SIMD-gated fast path
 * (TODO.impl/05; TODO.restructure/69). Clean 32-byte chunks ride the
 * gate_scan kernel (0 = every byte printable ASCII — validated in one
 * pass); dirty chunks fall to the exact per-byte walk, which resumes
 * the kernel at the next sequence boundary. The original SWAR path
 * broke at the first non-ASCII byte and never recovered.
 *
 * The well-formedness face (yep_utf8_validate) rides a byte-DFA first
 * (issue #742): one table load per byte answers the dirty-span case,
 * with the gate kernel still taking the all-ASCII span in one call.
 * The exact walk remains the authority on rejections (err_pos).
 *
 * Rules enforced (Unicode 15 well-formedness): no overlong encodings, no
 * UTF-16 surrogate code points (U+D800..U+DFFF), nothing above U+10FFFF,
 * continuation bytes only where a sequence expects them.
 *
 * yep_printable_validate additionally enforces the YAML 1.2 c-printable
 * charset on the stream (the charset SSOT lives here): raw C0 controls
 * except TAB/LF/CR, DEL, C1 except NEL (U+0085), and U+FFFE/U+FFFF are
 * rejected wherever they appear. Escape sequences are decoded later —
 * printable applies to source bytes only.
 */

#include <stdint.h>
#include <string.h>

#include "../common/simd_text.h"
#include "encoding.h"

static int ascii_ok(unsigned char b, int printable) {
    if (!printable) {
        return 1;
    }
    /* c-printable ASCII: TAB, LF, CR, 0x20..0x7E */
    if (b == '\t' || b == '\n' || b == '\r') {
        return 1;
    }
    return b >= 0x20 && b <= 0x7E;
}

/* Code point >= 0x80 allowed by c-printable? (surrogates are already
 * rejected by the well-formedness rules above). */
static int cp_printable(uint32_t cp) {
    return cp == 0x85 || (cp >= 0xA0 && cp <= 0xFFFD) || cp >= 0x10000;
}

/* The exact per-byte walk: ASCII gate check plus the multibyte DFA.
 * Advances *i by one byte or one whole sequence; returns 0 on error
 * (with *err_pos set), 1 to continue. */
static int step(const unsigned char* p, size_t len, size_t* i, size_t* err_pos, int printable) {
    size_t at = *i;
    unsigned char b = p[at];
    if (b < 0x80) {
        if (!ascii_ok(b, printable)) {
            if (err_pos != NULL) {
                *err_pos = at;
            }
            return 0;
        }
        *i = at + 1;
        return 1;
    }

    size_t need;          /* continuation bytes expected */
    unsigned char lo, hi; /* allowed range of the first continuation */
    if (b >= 0xC2 && b <= 0xDF) {
        need = 1;
        lo = 0x80;
        hi = 0xBF;
    } else if (b == 0xE0) {
        need = 2;
        lo = 0xA0; /* no overlong two-byte */
        hi = 0xBF;
    } else if ((b >= 0xE1 && b <= 0xEC) || b == 0xEE || b == 0xEF) {
        need = 2;
        lo = 0x80;
        hi = 0xBF;
    } else if (b == 0xED) {
        need = 2;
        lo = 0x80;
        hi = 0x9F; /* no surrogates */
    } else if (b == 0xF0) {
        need = 3;
        lo = 0x90; /* no overlong three-byte */
        hi = 0xBF;
    } else if (b >= 0xF1 && b <= 0xF3) {
        need = 3;
        lo = 0x80;
        hi = 0xBF;
    } else if (b == 0xF4) {
        need = 3;
        lo = 0x80;
        hi = 0x8F; /* no > U+10FFFF */
    } else {
        if (err_pos != NULL) {
            *err_pos = at;
        }
        return 0;
    }

    /* The sequence spans [at, at + need]; all those bytes must exist. */
    if (at + need >= len) {
        if (err_pos != NULL) {
            *err_pos = at;
        }
        return 0;
    }

    unsigned char c1 = p[at + 1];
    if (c1 < lo || c1 > hi) {
        if (err_pos != NULL) {
            *err_pos = at + 1;
        }
        return 0;
    }
    for (size_t k = 2; k <= need; k++) {
        if (p[at + k] < 0x80 || p[at + k] > 0xBF) {
            if (err_pos != NULL) {
                *err_pos = at + k;
            }
            return 0;
        }
    }
    if (printable) {
        uint32_t cp;
        if (need == 1) {
            cp = ((uint32_t)(b & 0x1F) << 6) | (uint32_t)(c1 & 0x3F);
        } else if (need == 2) {
            cp = ((uint32_t)(b & 0x0F) << 12) | ((uint32_t)(c1 & 0x3F) << 6) |
                 (uint32_t)(p[at + 2] & 0x3F);
        } else {
            cp = ((uint32_t)(b & 0x07) << 18) | ((uint32_t)(c1 & 0x3F) << 12) |
                 ((uint32_t)(p[at + 2] & 0x3F) << 6) | (uint32_t)(p[at + 3] & 0x3F);
        }
        if (!cp_printable(cp)) {
            if (err_pos != NULL) {
                *err_pos = at;
            }
            return 0;
        }
    }
    *i = at + 1 + need;
    return 1;
}

static int validate(const unsigned char* p, size_t len, size_t* err_pos, int printable) {
    if (err_pos != NULL) {
        *err_pos = 0;
    }
    if (p == NULL && len != 0) {
        return 0;
    }

    size_t i = 0;

    while (i + 32 <= len) {
        if (yep_text_active()->gate_scan((const char*)p + i, 32) == 0) {
            i += 32;
            continue;
        }
        size_t edge = i + 32;
        while (i < edge) {
            if (!step(p, len, &i, err_pos, printable)) {
                return 0;
            }
        }
    }

    while (i < len) {
        if (!step(p, len, &i, err_pos, printable)) {
            return 0;
        }
    }
    return 1;
}

/* ---- the well-formedness DFA fast path (issue #742) -------------------
 *
 * The CBOR decode profile puts ~40% of the scan-only lane in the
 * per-byte step walk: real corpora are ~3% high bytes but scattered,
 * so two thirds of the 32-byte gate windows go dirty and the exact
 * walk rules them byte by byte (nine-branch first-byte cascade plus
 * per-byte ascii_ok). The DFA answers each byte with ONE table load:
 * class (the byte's role) then next-state, with the overlong/
 * surrogate/U+10FFFF bounds encoded in the transitions — the same
 * rules step() enforces, in the same order along the span.
 *
 * Verdict equivalence is pinned three ways: the tables were derived
 * from step()'s range rules; a differential harness walked every
 * boundary sequence of length <= 4 (29 range-edge bytes), the full
 * two-byte alphabet, every (lead, continuation) pair for the
 * multi-byte leads, the truncation shapes, 1M random strings, and the
 * fuzz corpus — 0 mismatches; and test_encoding's pinned verdicts +
 * err_pos ride the suite. On any DFA rejection the authoritative walk
 * below re-runs for the pinned err_pos (invalid inputs take the exact
 * path; valid ones never do).
 *
 * The gate answers only the long spans (>= 128 B): short ones (CBOR
 * map keys are single-digit bytes) fall through the kernel's 16-byte
 * loop into its scalar per-byte walk, paying a dispatch lookup to get
 * there — the SWAR loop below answers those spans directly. */

static const uint8_t utf8_class[256] = {
    /* 00-7F ASCII */ 0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    /* 80-8F */ 1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    /* 90-9F */ 2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    2,
    /* A0-BF */ 3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    3,
    /* C0-C1 overlong, C2-DF two-byte */ 4,
    4,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    5,
    /* E0, E1-EC, ED, EE-EF */ 6,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    7,
    8,
    7,
    7,
    /* F0, F1-F3, F4, F5-FF */ 9,
    10,
    10,
    10,
    11,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
    12,
};

#define UTF8_ST_INVALID 255
/* 0 START; 1 expect one 80-BF; 2 expect two 80-BF; 3 E0->A0-BF;
 * 4 F0->90-BF; 5 ED->80-9F; 6 expect three 80-BF; 7 F4->80-8F.
 * Indexed [state * 16 + class]. */
static const uint8_t utf8_next[8 * 16] = {
    0,   255, 255, 255, 255, 1,   3,   2,   5,   4,   6,   7,   255, 255, 255, 255, 255, 0,   0,
    0,   255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 1,   1,   1,   255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 1,   255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 2,   2,   255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 1,   1,   255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 2,   2,   2,   255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 2,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
};

/* Verdict-identical to the printable=0 walk (the differential gates
 * above; the suite pins the observable). 1 = well-formed.
 *
 * The loop rides the input's shape: in START state the span's ASCII
 * runs (97% of real text) advance eight bytes per SWAR test with no
 * DFA traffic — the high-byte islands pay the table loads alone. The
 * per-byte branchy walk paid the nine-branch cascade per NON-ASCII
 * byte and a call per ASCII byte inside dirty windows; the DFA pays
 * two dependent table loads per non-ASCII byte and nothing else. */
static int validate_wellformed(const unsigned char* p, size_t len) {
    /* the gate pays off on long spans only: short ones (CBOR map keys
     * are single-digit bytes — the #742 profile) fall through the
     * kernel's 16-byte loop into its scalar per-byte walk, paying a
     * dispatch lookup to get there; the SWAR loop below answers those
     * spans directly */
    if (len >= 128 && yep_text_active()->gate_scan((const char*)p, len) == 0) {
        return 1; /* every byte gate-safe ASCII */
    }
    uint8_t st = 0;
    size_t j = 0;
    for (;;) {
        while (st == 0 && j + 8 <= len) { /* START-state ASCII runs, 8 bytes per test */
            uint64_t w;
            memcpy(&w, p + j, sizeof(w));
            if ((w & 0x8080808080808080ull) != 0) {
                break;
            }
            j += 8;
        }
        while (st == 0 && j < len && p[j] < 0x80) { /* the ASCII tail under 8 bytes */
            j++;
        }
        if (j >= len) {
            break;
        }
        st = utf8_next[(uint8_t)((st << 4) | utf8_class[p[j]])];
        if (st == UTF8_ST_INVALID) {
            return 0;
        }
        j++;
    }
    return st == 0;
}

int yep_utf8_validate(const unsigned char* p, size_t len, size_t* err_pos) {
    if (p == NULL && len != 0) {
        if (err_pos != NULL) {
            *err_pos = 0;
        }
        return 0;
    }
    if (validate_wellformed(p, len)) {
        if (err_pos != NULL) {
            *err_pos = 0;
        }
        return 1;
    }
    /* the DFA said no: the authoritative walk finds the byte (the
     * err_pos contract is pinned by test_encoding) */
    return validate(p, len, err_pos, 0);
}

int yep_printable_validate(const unsigned char* p, size_t len, size_t* err_pos) {
    return validate(p, len, err_pos, 1);
}

/* utf8_validate.c — well-formedness check with a SIMD-gated fast path
 * (TODO.impl/05; TODO.restructure/69). Clean 32-byte chunks ride the
 * gate_scan kernel (0 = every byte printable ASCII — validated in one
 * pass); dirty chunks fall to the exact per-byte walk, which resumes
 * the kernel at the next sequence boundary. The original SWAR path
 * broke at the first non-ASCII byte and never recovered.
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

int yep_utf8_validate(const unsigned char* p, size_t len, size_t* err_pos) {
    return validate(p, len, err_pos, 0);
}

int yep_printable_validate(const unsigned char* p, size_t len, size_t* err_pos) {
    return validate(p, len, err_pos, 1);
}

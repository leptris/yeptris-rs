/* chartype.h — THE byte classification truth table (MECE law: chartype
 * answers "what class of byte is this" and nothing else; scan (06) answers
 * "where structures start"; the SIMD kernels (04) implement these same
 * classes with vector compares, proven equivalent by differential tests).
 *
 * Byte-level approximation of the YAML character productions:
 *  - Printability is decided per byte for ASCII (tab/LF/CR/0x20-0x7E are
 *    printable, C0 controls and DEL are not). Bytes >= 0x80 carry HIGH;
 *    their printability is decided by the encoding front-end (05), which
 *    validates the whole sequence.
 *  - YAML's Unicode line breaks (NEL U+0085, LS U+2028, PS U+2029) are
 *    multi-byte; they are recognized by the scan layer, not by this table.
 *  - c-indicator set (YAML 1.2 spec, 5.5): - ? : , [ ] { } # & * ! | >
 *    ' " % @ `  — of these, `, [ ] { }` are flow indicators.
 *
 * Derived classes are inline helpers, not table bits: ns-char is
 * printable-and-not-whitespace (plus HIGH, pending encoding validation).
 */
#ifndef YEP_CHARTYPE_H
#define YEP_CHARTYPE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    YEP_CT_SPACE = 1u << 0,
    YEP_CT_TAB = 1u << 1,
    YEP_CT_LF = 1u << 2,
    YEP_CT_CR = 1u << 3,
    YEP_CT_DIGIT = 1u << 4,
    YEP_CT_ALPHA = 1u << 5,
    YEP_CT_HEXDIGIT = 1u << 6,
    YEP_CT_INDICATOR = 1u << 7, /* the 19-byte c-indicator set */
    YEP_CT_FLOW_IND = 1u << 8,  /* , [ ] { } */
    YEP_CT_HIGH = 1u << 9,      /* >= 0x80: UTF-8 sequence byte */

    /* Composite (stored pre-computed in the table). */
    YEP_CT_BLANK = YEP_CT_SPACE | YEP_CT_TAB,
    YEP_CT_LBREAK = YEP_CT_LF | YEP_CT_CR,
    YEP_CT_PRINTABLE = 1u << 10, /* byte-level: tab|LF|CR|0x20..0x7E */
};

/* The 256-entry truth table (const, rodata). */
extern const uint16_t yep_chartype_table[256];

static inline uint16_t yep_ct(unsigned char c) {
    return yep_chartype_table[c];
}

/* True when ALL of flags are set on c. */
static inline bool yep_ct_is(unsigned char c, uint16_t flags) {
    return (yep_chartype_table[c] & flags) == flags;
}

static inline bool yep_ct_any(unsigned char c, uint16_t flags) {
    return (yep_chartype_table[c] & flags) != 0;
}

/* ns-char approximation: printable and not whitespace; high bytes defer to
 * the encoding layer. */
static inline bool yep_ct_is_ns(unsigned char c) {
    uint16_t t = yep_chartype_table[c];
    if (t & YEP_CT_HIGH) {
        return true;
    }
    return (t & YEP_CT_PRINTABLE) != 0 && (t & (YEP_CT_BLANK | YEP_CT_LBREAK)) == 0;
}

#ifdef __cplusplus
}
#endif

#endif /* YEP_CHARTYPE_H */

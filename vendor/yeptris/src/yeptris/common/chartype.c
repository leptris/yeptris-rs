/* chartype.c — the 256-entry byte classification table.
 *
 * Row macros (class combos actually used); the golden test
 * (test/unit/test_chartype.cpp) re-derives every entry from the spec
 * productions, so a typo here fails the suite.
 */

#include "chartype.h"

#define Z 0
#define BT (YEP_CT_TAB | YEP_CT_BLANK | YEP_CT_PRINTABLE)
#define BL (YEP_CT_LF | YEP_CT_LBREAK | YEP_CT_PRINTABLE)
#define BC (YEP_CT_CR | YEP_CT_LBREAK | YEP_CT_PRINTABLE)
#define BS (YEP_CT_SPACE | YEP_CT_BLANK | YEP_CT_PRINTABLE)
#define BI (YEP_CT_INDICATOR | YEP_CT_PRINTABLE)
#define BF (YEP_CT_FLOW_IND | YEP_CT_INDICATOR | YEP_CT_PRINTABLE)
#define BP (YEP_CT_PRINTABLE)
#define BD (YEP_CT_DIGIT | YEP_CT_HEXDIGIT | YEP_CT_PRINTABLE)
#define BA (YEP_CT_ALPHA | YEP_CT_PRINTABLE)
#define BX (YEP_CT_ALPHA | YEP_CT_HEXDIGIT | YEP_CT_PRINTABLE)
#define BH (YEP_CT_HIGH)

const uint16_t yep_chartype_table[256] = {
    /*        0x00 0x01 0x02 0x03 0x04 0x05 0x06 0x07 0x08 0x09 0x0A 0x0B 0x0C 0x0D 0x0E 0x0F */
    /* 0x00 */ Z,  Z,  Z,  Z,  Z,  Z,  Z,  Z,  Z,  BT, BL, Z,  Z,  BC, Z,  Z,
    /* 0x10 */ Z,  Z,  Z,  Z,  Z,  Z,  Z,  Z,  Z,  Z,  Z,  Z,  Z,  Z,  Z,  Z,
    /* 0x20 */ BS, BI, BI, BI, BP, BI, BI, BI, BP, BP, BI, BP, BF, BI, BP, BP,
    /* 0x30 */ BD, BD, BD, BD, BD, BD, BD, BD, BD, BD, BI, BP, BP, BP, BI, BI,
    /* 0x40 */ BI, BX, BX, BX, BX, BX, BX, BA, BA, BA, BA, BA, BA, BA, BA, BA,
    /* 0x50 */ BA, BA, BA, BA, BA, BA, BA, BA, BA, BA, BA, BF, BP, BF, BP, BP,
    /* 0x60 */ BI, BX, BX, BX, BX, BX, BX, BA, BA, BA, BA, BA, BA, BA, BA, BA,
    /* 0x70 */ BA, BA, BA, BA, BA, BA, BA, BA, BA, BA, BA, BF, BI, BF, BP, Z,
    /* 0x80 */ BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH,
    /* 0x90 */ BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH,
    /* 0xA0 */ BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH,
    /* 0xB0 */ BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH,
    /* 0xC0 */ BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH,
    /* 0xD0 */ BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH,
    /* 0xE0 */ BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH,
    /* 0xF0 */ BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH, BH,
};

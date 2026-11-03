/* bom.c — byte-order-mark sniffing (TODO.impl/05). */

#include "encoding.h"

size_t yep_bom_sniff(const unsigned char* p, size_t len, yep_encoding* out) {
    yep_encoding enc = YEP_ENC_UNKNOWN;
    size_t consumed = 0;

    /* UTF-32LE (FF FE 00 00) must be tested before UTF-16LE (FF FE). */
    if (len >= 4 && p[0] == 0xFF && p[1] == 0xFE && p[2] == 0x00 && p[3] == 0x00) {
        enc = YEP_ENC_UTF32LE;
        consumed = 4;
    } else if (len >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {
        enc = YEP_ENC_UTF8;
        consumed = 3;
    } else if (len >= 4 && p[0] == 0x00 && p[1] == 0x00 && p[2] == 0xFE && p[3] == 0xFF) {
        enc = YEP_ENC_UTF32BE;
        consumed = 4;
    } else if (len >= 2 && p[0] == 0xFF && p[1] == 0xFE) {
        enc = YEP_ENC_UTF16LE;
        consumed = 2;
    } else if (len >= 2 && p[0] == 0xFE && p[1] == 0xFF) {
        enc = YEP_ENC_UTF16BE;
        consumed = 2;
    }

    if (out != NULL) {
        *out = enc;
    }
    return consumed;
}

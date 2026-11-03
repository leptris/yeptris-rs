/* transcode.c — UTF-16/UTF-32 (either endianness) → UTF-8 (TODO.impl/05).
 *
 * Strict v1: ill-formed input (unpaired surrogates, values above
 * U+10FFFF, truncated units) is an error at the offending unit; the
 * compat replacement policy arrives with parse options (10).
 * Worst-case sizing: UTF-16 unit 2 bytes → 3 UTF-8 bytes (1.5x);
 * UTF-32 unit 4 → 4 bytes (1x).
 */

#include <stdint.h>

#include "encoding.h"

static unsigned char* yep_emit_utf8(unsigned char* o, uint32_t cp) {
    if (cp < 0x80) {
        *o++ = (unsigned char)cp;
    } else if (cp < 0x800) {
        *o++ = (unsigned char)(0xC0 | (cp >> 6));
        *o++ = (unsigned char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *o++ = (unsigned char)(0xE0 | (cp >> 12));
        *o++ = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (unsigned char)(0x80 | (cp & 0x3F));
    } else {
        *o++ = (unsigned char)(0xF0 | (cp >> 18));
        *o++ = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        *o++ = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (unsigned char)(0x80 | (cp & 0x3F));
    }
    return o;
}

static uint16_t rd16(const unsigned char* p, int big_endian) {
    return big_endian ? (uint16_t)((p[0] << 8) | p[1]) : (uint16_t)((p[1] << 8) | p[0]);
}

static int yep_is_surrogate(uint32_t cp) {
    return cp >= 0xD800 && cp <= 0xDFFF;
}

static int yep_transcode_utf16(const yep_allocator* sys, int big_endian, const unsigned char* src,
                               size_t len, unsigned char** out, size_t* out_len, size_t* err_pos) {
    if (len % 2 != 0) { /* trailing partial unit */
        if (err_pos != NULL) {
            *err_pos = len - 1;
        }
        return -2;
    }

    size_t units = len / 2;
    size_t max_out = units * 3;
    unsigned char* dst = yep_alloc(sys, max_out ? max_out : 1);
    if (dst == NULL) {
        return -1;
    }

    unsigned char* o = dst;
    for (size_t i = 0; i < units; i++) {
        uint16_t u = rd16(src + i * 2, big_endian);
        if (u >= 0xD800 && u <= 0xDBFF) {
            if (i + 1 >= units) {
                if (err_pos != NULL) {
                    *err_pos = i * 2;
                }
                yep_free(sys, dst);
                return -2; /* lone high surrogate */
            }
            uint16_t v = rd16(src + (i + 1) * 2, big_endian);
            if (v < 0xDC00 || v > 0xDFFF) {
                if (err_pos != NULL) {
                    *err_pos = (i + 1) * 2;
                }
                yep_free(sys, dst);
                return -2; /* high surrogate not followed by low */
            }
            uint32_t cp = 0x10000 + (((uint32_t)(u - 0xD800)) << 10) + (uint32_t)(v - 0xDC00);
            o = yep_emit_utf8(o, cp);
            i++;
        } else if (u >= 0xDC00 && u <= 0xDFFF) {
            if (err_pos != NULL) {
                *err_pos = i * 2;
            }
            yep_free(sys, dst);
            return -2; /* lone low surrogate */
        } else {
            o = yep_emit_utf8(o, u);
        }
    }

    *out = dst;
    *out_len = (size_t)(o - dst);
    return 0;
}

static int yep_transcode_utf32(const yep_allocator* sys, int big_endian, const unsigned char* src,
                               size_t len, unsigned char** out, size_t* out_len, size_t* err_pos) {
    size_t units = len / 4;
    if (len % 4 != 0) {
        /* a dangling final unit is an error, not a silent truncation
         * (the UTF-16 twin rejects its own; the header contract says
         * truncated units error at the offending byte) */
        if (err_pos != NULL) {
            *err_pos = units * 4;
        }
        return -2;
    }
    unsigned char* dst = yep_alloc(sys, len ? len : 1);
    if (dst == NULL) {
        return -1;
    }

    unsigned char* o = dst;
    for (size_t i = 0; i < units; i++) {
        const unsigned char* p = src + i * 4;
        uint32_t cp = big_endian ? ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                                       ((uint32_t)p[2] << 8) | (uint32_t)p[3]
                                 : ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
                                       ((uint32_t)p[1] << 8) | (uint32_t)p[0];
        if (cp > 0x10FFFF || yep_is_surrogate(cp)) {
            if (err_pos != NULL) {
                *err_pos = i * 4;
            }
            yep_free(sys, dst);
            return -2;
        }
        o = yep_emit_utf8(o, cp);
    }

    *out = dst;
    *out_len = (size_t)(o - dst);
    return 0;
}

int yep_transcode_to_utf8(const yep_allocator* sys, yep_encoding enc, const unsigned char* src,
                          size_t len, unsigned char** out, size_t* out_len, size_t* err_pos) {
    if (sys == NULL || out == NULL || out_len == NULL || (src == NULL && len != 0)) {
        return -1;
    }
    if (len == 0) {
        *out = NULL;
        *out_len = 0;
        return 0;
    }

    switch (enc) {
    case YEP_ENC_UTF16LE:
        return yep_transcode_utf16(sys, 0, src, len, out, out_len, err_pos);
    case YEP_ENC_UTF16BE:
        return yep_transcode_utf16(sys, 1, src, len, out, out_len, err_pos);
    case YEP_ENC_UTF32LE:
        return yep_transcode_utf32(sys, 0, src, len, out, out_len, err_pos);
    case YEP_ENC_UTF32BE:
        return yep_transcode_utf32(sys, 1, src, len, out, out_len, err_pos);
    default:
        return -2; /* not a transcoding source */
    }
}

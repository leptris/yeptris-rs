/* encoding.h — the encoding front-end (TODO.impl/05).
 *
 * MECE law: charset truth lives ONLY here. The scan layer receives a
 * buffer guaranteed valid UTF-8 and never re-validates; output is always
 * UTF-8 (the libleptris serialization guarantee).
 *
 * Zero-copy policy: UTF-8 input is validated in place and borrowed;
 * non-UTF-8 input is transcoded once (into a buffer allocated via the
 * caller's allocator — item 07/11 re-home it into the document arena).
 *
 * v1 scope decisions (recorded in TODO.impl/05): validation is scalar
 * with a SWAR ASCII fast path — the SIMD kernel lands only if item 06's
 * profiles demand it; transcoding rejects ill-formed input (strict) —
 * compat replacement policy arrives with the parse-options item (10).
 */
#ifndef YEP_ENCODING_H
#define YEP_ENCODING_H

#include <stddef.h>

#include "memory/allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    YEP_ENC_UNKNOWN = 0,
    YEP_ENC_UTF8,
    YEP_ENC_UTF16LE,
    YEP_ENC_UTF16BE,
    YEP_ENC_UTF32LE,
    YEP_ENC_UTF32BE,
} yep_encoding;

/* Sniffs a byte-order mark. Sets *out (UNKNOWN when none) and returns the
 * BOM length in bytes (0 when none). Partial BOMs at end-of-input match
 * nothing. UTF-32LE is checked before UTF-16LE (FF FE 00 00 is both).
 * out may be NULL. */
size_t yep_bom_sniff(const unsigned char* p, size_t len, yep_encoding* out);

/* Validates UTF-8: 1 when [p, p+len) is well-formed (no overlongs, no
 * surrogates, no > U+10FFFF, no truncation), else 0 with *err_pos set to
 * the offset of the first offending byte (err_pos may be NULL). */
int yep_utf8_validate(const unsigned char* p, size_t len, size_t* err_pos);

/* Well-formed UTF-8 AND the YAML 1.2 c-printable charset: raw C0
 * controls except TAB/LF/CR, DEL, C1 except NEL, U+FFFE/U+FFFF are
 * rejected (charset SSOT — encoding/ is the only place that decides). */
int yep_printable_validate(const unsigned char* p, size_t len, size_t* err_pos);

/* Transcodes UTF-16/32 (any endianness) into freshly allocated UTF-8.
 * On success returns 0, stores the buffer (owned by the caller, freed
 * with the allocator) and its length. Errors: -1 allocation failure,
 * -2 ill-formed input with *err_pos at the offending unit offset. */
int yep_transcode_to_utf8(const yep_allocator* sys, yep_encoding enc, const unsigned char* src,
                          size_t len, unsigned char** out, size_t* out_len, size_t* err_pos);

#ifdef __cplusplus
}
#endif

#endif /* YEP_ENCODING_H */

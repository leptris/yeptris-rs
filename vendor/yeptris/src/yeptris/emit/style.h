/* style.h — emission style rules (TODO.impl/13).
 *
 * The parse-recorded style is the SSOT of "what this scalar is like";
 * these rules decide re-emission safety and fresh-value choices. A
 * rule is a predicate over bytes — the table, not a branch soup, is
 * where new rules land (OCP).
 */
#ifndef YEP_EMIT_STYLE_H
#define YEP_EMIT_STYLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A plain scalar may be re-emitted plain only when every byte is safe
 * in ANY position of a block context: no leading/trailing space, no
 * indicator first byte, no ": " / " #" inside, no tab, no break, and
 * the value is not a document marker or the empty-string corner that
 * needs quotes (empty plain emits as an empty value instead). */
int yep_style_plain_safe(const char* p, uint32_t len);

/* A plain scalar is key-safe when it may sit directly before ':'
 * (plain_safe plus: no ':' at all unless followed by a non-blank). */
int yep_style_plain_key_safe(const char* p, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* YEP_EMIT_STYLE_H */

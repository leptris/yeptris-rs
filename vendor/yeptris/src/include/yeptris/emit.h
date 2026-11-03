/* emit.h — serialization (TODO.impl/13).
 *
 * One writer engine serves DOM walks and (with 13C) event streams;
 * sizing runs the SAME engine dry, so the size query and the write
 * agree by construction — zero reallocations after the reserve.
 *
 * Roundtrip fidelity: re-serializing a parsed document re-emits its
 * recorded scalar styles (a plain stays plain where safe, quoted stays
 * quoted, block scalars stay block). Byte stability:
 * serialize(parse(serialize(x))) == serialize(x) — gated by the
 * corpus roundtrip test.
 */
#ifndef YEPTRIS_EMIT_H
#define YEPTRIS_EMIT_H

#include <stddef.h>
#include <stdint.h>

#include <yeptris/api.h>
#include <yeptris/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Emission options (13B). Versioned by size: initialize with
 * {sizeof(yeptris_emit_options)} so future fields stay optional. */
typedef struct yeptris_emit_options {
    uint32_t size;          /* sizeof(yeptris_emit_options) */
    int canonical;          /* fixed form: flow collections, quoted strings,
                             * typed words, shortest floats; a parse of the
                             * output re-serializes byte-identically */
    int best_width;         /* 0 = library default (80): flow collections
                             * wrap after a value when the current line
                             * exceeds the width (13B) */
    int explicit_doc_start; /* nonzero: every document carries the
                             * leading --- marker (Psych/libyaml's
                             * implicit=false parity). Default 0: a
                             * single document is emitted bare. Only
                             * read when the caller's struct is large
                             * enough (size-versioned). */
} yeptris_emit_options;

/* Options-aware entries; opts == NULL selects the fidelity mode
 * (recorded styles re-emitted). Same contracts as below. */
YEPTRIS_API size_t yeptris_serialize_into_ex(YeptrisDocument doc, const yeptris_emit_options* opts,
                                             char* buf, size_t cap);
YEPTRIS_API char* yeptris_serialize_ex(YeptrisDocument doc, const yeptris_emit_options* opts,
                                       size_t* len);

/* Serializes the document set. buf == NULL: returns the exact byte
 * count needed and writes nothing. Otherwise writes at most cap bytes
 * (NUL-terminated when cap allows) and returns the count written;
 * when cap is too small nothing is written and the needed size is
 * returned. Returns 0 on a NULL document. */
YEPTRIS_API size_t yeptris_serialize_into(YeptrisDocument doc, char* buf, size_t cap);

/* Trusted-cap emission (#352): ONE write pass into the caller's
 * fixed buffer — no sizing pass, no per-call allocation. Returns the
 * byte count written when ret < cap (the document is complete and
 * NUL-terminated at buf[ret]); when ret >= cap the output did not
 * fit — buf holds a partial document and ret is the exact total the
 * document needs (re-call with cap >= ret + 1; the retry writes
 * identical bytes). opts == NULL selects the fidelity mode. Returns 0
 * on a NULL document or buffer (use yeptris_serialize_into for the
 * measure-only form). */
YEPTRIS_API size_t yeptris_serialize_into_trusted(YeptrisDocument doc,
                                                  const yeptris_emit_options* opts, char* buf,
                                                  size_t cap);

/* Streaming emission (13C): bytes are delivered in chunks to sink as
 * the walk proceeds — memory bounded by the largest scalar, never the
 * document. Returns the total byte count, or 0 on a NULL argument or
 * when the sink aborts (nonzero return aborts the walk). */
typedef int (*yeptris_emit_sink)(void* ctx, const char* bytes, size_t len);
YEPTRIS_API size_t yeptris_serialize_stream(YeptrisDocument doc, const yeptris_emit_options* opts,
                                            yeptris_emit_sink sink, void* ctx);

/* Convenience: serializes into a malloc'd buffer (caller frees).
 * Returns NULL on allocation failure or a NULL document; *len (may be
 * NULL) receives the byte count. The buffer is NUL-terminated. */
YEPTRIS_API char* yeptris_serialize(YeptrisDocument doc, size_t* len);

/* Frees a buffer the library returned (the serialize* family's
 * "caller frees" contract). The buffer was allocated by THIS
 * library's allocator — freeing it with the host's free is
 * wrong-CRT on Windows (the #318 mingw lesson). NULL is a no-op. */
YEPTRIS_API void yeptris_free(void* p);

#ifdef __cplusplus
}
#endif

#endif /* YEPTRIS_EMIT_H */

/* jsonc_compat.h — the json-c migration layer (TODO.impl/21).
 *
 * REAL json-c symbol names over the yeptris core: users link
 * -lyeptris-jsonc instead of -ljson-c and change nothing else.
 *
 * Scope (v1, the read path): parsing, type queries, value getters,
 * object/array access, and JSON output. The building API
 * (json_object_new_ etc.) landed with DOM mutation
 * (TODO.impl/11 phase 3): the new_object family, object_add,
 * array_add, and the delete entry points.
 *
 * Lifetime: the object returned by json_tokener_parse owns the
 * document; json_object_put frees it. Objects obtained by reference
 * (children) are owned by their document — put on them is a no-op
 * returning 1 (json-c children outlive callers the same way).
 */
#ifndef YEPTRIS_JSONC_COMPAT_H
#define YEPTRIS_JSONC_COMPAT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Values mirror json-c's json_type. */
typedef enum {
    json_type_null = 0,
    json_type_boolean,
    json_type_double,
    json_type_int,
    json_type_object,
    json_type_array,
    json_type_string
} json_type;

typedef struct json_object json_object;

/* json-c's string flags for to_json_string_ext */
#define JSON_C_TO_STRING_PLAIN 0
#define JSON_C_TO_STRING_SPACED (1 << 0)
#define JSON_C_TO_STRING_PRETTY (1 << 1)
#define JSON_C_TO_STRING_PRETTY_TAB (1 << 2)
#define JSON_C_TO_STRING_NOZERO (1 << 3)

/* Parses one JSON document (strict; trailing whitespace allowed).
 * Returns NULL on invalid input. The result owns its document —
 * release with json_object_put. */
json_object* json_tokener_parse(const char* str);
json_object* json_tokener_parse_ex(const char* buf, int len);
void json_tokener_free(json_object* obj);

/* Lifetime: put on the ROOT frees the document (once); on a child it
 * is a no-op returning 1. get returns the object unchanged. */
int json_object_put(json_object* obj);
json_object* json_object_get(json_object* obj);

/* Type queries. */
json_type json_object_get_type(const json_object* obj);
int json_object_is_type(const json_object* obj, json_type type);

/* Value getters (0 when the type does not match, like json-c). */
const char* json_object_get_string(json_object* obj);
int32_t json_object_get_int(const json_object* obj);
int64_t json_object_get_int64(const json_object* obj);
double json_object_get_double(const json_object* obj);
int json_object_get_boolean(const json_object* obj);

/* Object access: *value receives the field (a borrowed reference). */
int json_object_object_get_ex(const json_object* obj, const char* key, json_object** value);
size_t json_object_object_length(const json_object* obj);

/* Array access (borrowed references). */
size_t json_object_array_length(const json_object* obj);
json_object* json_object_array_get_idx(const json_object* obj, size_t idx);

/* Output: strict JSON. The returned pointer is owned by the object
 * (freed with it) — json-c's contract. Roots only in v1: serializing
 * an attached child returns NULL (subtree output arrives with the
 * pretty printer). */
const char* json_object_to_json_string(json_object* obj);
const char* json_object_to_json_string_ext(json_object* obj, int flags);

/* ---- Building (TODO.impl/21 v2, over DOM mutation 11/3) ----
 *
 * json-c semantics: new_* returns an unowned object; adding it to a
 * parent transfers ownership (a put on it afterwards is a no-op); the
 * caller's pointer stays valid until the ROOT is put. Values are
 * copied. Duplicate keys replace in place (position kept). NULL val
 * to object_add deletes the key (legacy json-c behavior). */
json_object* json_object_new_object(void);
json_object* json_object_new_array(void);
json_object* json_object_new_string(const char* s);
json_object* json_object_new_string_len(const char* s, int len);
json_object* json_object_new_int(int32_t i);
json_object* json_object_new_int64(int64_t i);
json_object* json_object_new_double(double d);
json_object* json_object_new_boolean(int b);

/* 0 added, 1 replaced, -1 error (obj not an object, or OOM). */
int json_object_object_add(json_object* obj, const char* key, json_object* val);
int json_object_object_del(json_object* obj, const char* key);

/* 0 ok, -1 error (obj not an array, or OOM). */
int json_object_array_add(json_object* obj, json_object* val);
int json_object_array_del_idx(json_object* obj, size_t idx, size_t count);

/* index < len replaces in place; index == len appends. */
int json_object_array_put_idx(json_object* obj, size_t idx, json_object* val);

#ifdef __cplusplus
}
#endif

#endif /* YEPTRIS_JSONC_COMPAT_H */

/* resolver.h — the internal resolver interface (TODO.impl/10).
 *
 * MECE: scan produces facts, the engine parses, the RESOLVER alone
 * decides implicit typing. OCP: a new schema is a new resolver
 * instance; no schema branches exist anywhere else.
 */
#ifndef YEP_RESOLVER_H
#define YEP_RESOLVER_H

#include <stdint.h>

#include "common/string_view.h"

/* yep_view: bytes + length, the one string representation (SSOT). */

/* The tag-id space is the public YeptrisTagId (resolve.h); internal
 * re-export for the table SSOT. */
typedef uint8_t yep_tag_id;

typedef struct yep_resolver {
    /* Returns the implicit tag for a PLAIN, untagged scalar. */
    yep_tag_id (*resolve)(void* ctx, const char* p, uint32_t len);
    /* Optional fast path for spans a validator already classified as
     * strict numbers (is_float: the text contains '.' or an exponent):
     * skips the digit re-walk resolve() would pay. NULL = resolve(). */
    yep_tag_id (*resolve_number)(void* ctx, int is_float);
    void* ctx;
    /* 1 when timestamp:yaml.org,2002 is IN the family's implicit set —
     * the validator family-checks explicit !!timestamp content through
     * resolve() (#906 slice 3). Core leaves it consumer-defined (the
     * core schema has no timestamp, spec 10.3); 11_COMPAT is not a
     * validation family; json12 rejects the tag wholesale. */
    int timestamped;
} yep_resolver;

/* Typing for a validated number span, through whichever hook the
 * schema provides (the typing SSOT either way). */
static inline yep_tag_id yep_resolve_number(const yep_resolver* r, int is_float) {
    if (r->resolve_number != NULL) {
        return r->resolve_number(NULL, is_float);
    }
    return is_float ? 2 /* float */ : 1 /* int */;
}

/* The two built-in resolvers (no ctx). */
const yep_resolver* yep_resolver_core12(void);
const yep_resolver* yep_resolver_compat11(void);
/* The core schema's plain-scalar classifier (the core truth table):
 * the resolver wraps it and the full family composes it. */
yep_tag_id yep_core12_plain(const char* p, uint32_t n);

/* The spec 10.2 JSON schema resolver (#882): null/true/false exact,
 * the JSON int and float productions, everything else str. Parse stays
 * lenient — the VALIDATOR flags plain scalars that matched no
 * production (the spec's `*` -> Error row) through the classifier. */
const yep_resolver* yep_resolver_json12(void);
/* 1 when the plain span matches a JSON-schema production. The one
 * truth table for the 12_JSON family (validate/ re-asks it). */
int yep_json12_plain(const char* p, uint32_t n);

/* The FULL 1.2 family resolver (#906 slice 3): the core productions
 * verbatim (spec 10.4: interoperable schemas are "strongly recommended
 * ... based on the core schema") extended with the yaml.org type
 * repository's carried-forward set — RFC3339-era timestamps on untagged
 * plains (spec 2.22's canonical/iso8601/spaced/date forms; PyYAML's
 * timestamp regex as the practice oracle) and the merge/value words.
 * The 1.1 NUMERIC quirks (underscores, sexagesimal, leading-0 octal,
 * y/n booleans) stay in compat11 — full is spec-refereed, not
 * psych-refereed. */
const yep_resolver* yep_resolver_full12(void);

/* core tag id <-> canonical URI (tags.c is the identity SSOT). */
const char* yep_tag_uri(yep_tag_id id);

/* Explicit-tag resolution: the tag URI a node carried maps to a core
 * id when it matches one, else YEPTRIS_TAG_CUSTOM. */
yep_tag_id yep_tag_from_uri(const char* p, uint32_t len);

/* Canonical word for a resolved float view: ".inf" / "-.inf" / ".nan"
 * or NULL when the view is not an infinity/nan word (the canonical
 * emitter re-prints these words; libc strtod does not accept them
 * portably). */
const char* yep_tag_float_word(const char* p, uint32_t len);

#endif /* YEP_RESOLVER_H */

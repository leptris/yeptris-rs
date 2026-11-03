/* yaml_compat.h — the libyaml-shaped event adapter (TODO.impl/12C).
 *
 * A porting shim: engine events translated into libyaml's event
 * surface (types, styles, implicit flags, marks) so libyaml test
 * drivers port against yeptris without touching their event code.
 * Internal until 17's drivers prove the shape complete — then it may
 * be promoted to a public compat header. Naming: yep_ly_* keeps our
 * law (internal = yep_*); enum VALUES mirror libyaml's exactly so
 * driver switch-statements port verbatim.
 *
 * Lifetime: anchor/tag/value are borrowed views into the parse input
 * or the engine finish pool — valid until the engine is destroyed
 * (same contract as the engine's own events). end_mark is lossy: the
 * engine records node starts only; end == start.
 */
#ifndef YEP_YAML_COMPAT_H
#define YEP_YAML_COMPAT_H

#include <stddef.h>

#include "parse/events.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Values mirror yaml_event_type_t (libyaml include/yaml.h). */
typedef enum {
    YEP_LY_NO_EVENT = 0,
    YEP_LY_STREAM_START_EVENT,
    YEP_LY_STREAM_END_EVENT,
    YEP_LY_DOCUMENT_START_EVENT,
    YEP_LY_DOCUMENT_END_EVENT,
    YEP_LY_ALIAS_EVENT,
    YEP_LY_SCALAR_EVENT,
    YEP_LY_SEQUENCE_START_EVENT,
    YEP_LY_SEQUENCE_END_EVENT,
    YEP_LY_MAPPING_START_EVENT,
    YEP_LY_MAPPING_END_EVENT
} yep_ly_event_type;

/* Values mirror yaml_scalar_style_t / yaml_sequence_style_t. */
enum {
    YEP_LY_ANY_STYLE = 0,
    YEP_LY_PLAIN_STYLE = 1,
    YEP_LY_SINGLE_QUOTED_STYLE = 2,
    YEP_LY_DOUBLE_QUOTED_STYLE = 3,
    YEP_LY_LITERAL_STYLE = 4,
    YEP_LY_FOLDED_STYLE = 5,
    YEP_LY_BLOCK_STYLE = 1,
    YEP_LY_FLOW_STYLE = 2
};

typedef struct yep_ly_mark {
    size_t index; /* byte offset; 0 here (the engine is offset-free) */
    size_t line;  /* 0-based, like libyaml */
    size_t column;
} yep_ly_mark;

typedef struct yep_ly_event {
    yep_ly_event_type type;

    /* scalar */
    const char* anchor; /* NULL when absent (views are NUL-safe pairs) */
    size_t anchor_len;
    const char* tag;
    size_t tag_len;
    const char* value;
    size_t value_len;
    int plain_implicit;  /* the tag may be omitted for a plain scalar */
    int quoted_implicit; /* the tag may be omitted for any quoted scalar */
    int implicit;        /* collections/documents: marker/tag optional */

    int encoding; /* stream_start: always UTF8 (value 1, like libyaml) */
    int style;    /* scalar/collection style, YEP_LY_*_STYLE */

    yep_ly_mark start_mark;
    yep_ly_mark end_mark; /* lossy: == start_mark */
} yep_ly_event;

/* Translates one engine event; returns 0 on success, -1 on a NULL
 * argument or an unknown event type. anchor/tag/value views borrow
 * the engine event's storage. */
int yep_ly_translate(yep_ly_event* out, const yep_event* in);

#ifdef __cplusplus
}
#endif

#endif /* YEP_YAML_COMPAT_H */

/* yaml_compat.c — engine events → libyaml event shape (TODO.impl/12C).
 *
 * The implicit-flag mapping mirrors what libyaml's own parser emits:
 * a scalar without a tag carries plain_implicit (plain) or
 * quoted_implicit (quoted); a collection without a tag is implicit.
 */

#include "events/yaml_compat.h"

#include <string.h>

int yep_ly_translate(yep_ly_event* out, const yep_event* in) {
    if (out == NULL || in == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->start_mark.line = in->line > 0 ? (size_t)in->line - 1 : 0;
    out->start_mark.column = in->col > 0 ? (size_t)in->col - 1 : 0;
    out->end_mark = out->start_mark;

    switch (in->type) {
    case YEP_EV_STREAM_START:
        out->type = YEP_LY_STREAM_START_EVENT;
        out->encoding = 1; /* UTF8 */
        return 0;
    case YEP_EV_STREAM_END:
        out->type = YEP_LY_STREAM_END_EVENT;
        return 0;
    case YEP_EV_DOCUMENT_START:
        out->type = YEP_LY_DOCUMENT_START_EVENT;
        out->implicit = (in->style != 1); /* style 1 = explicit "---" */
        return 0;
    case YEP_EV_DOCUMENT_END:
        out->type = YEP_LY_DOCUMENT_END_EVENT;
        out->implicit = (in->style != 1); /* style 1 = explicit "..." */
        return 0;
    case YEP_EV_ALIAS:
        out->type = YEP_LY_ALIAS_EVENT;
        out->anchor = (const char*)in->value.p;
        out->anchor_len = in->value.len;
        return 0;
    case YEP_EV_SCALAR:
        out->type = YEP_LY_SCALAR_EVENT;
        out->anchor = (const char*)in->anchor.p;
        out->anchor_len = in->anchor.len;
        out->tag = (const char*)in->tag.p;
        out->tag_len = in->tag.len;
        out->value = (const char*)in->value.p;
        out->value_len = in->value.len;
        out->style = in->style; /* YEP style values match libyaml's */
        if (in->tag.len == 0) {
            if (in->style == YEP_STYLE_PLAIN) {
                out->plain_implicit = 1;
            } else {
                out->quoted_implicit = 1;
            }
        }
        return 0;
    case YEP_EV_SEQ_START:
        out->type = YEP_LY_SEQUENCE_START_EVENT;
        out->anchor = (const char*)in->anchor.p;
        out->anchor_len = in->anchor.len;
        out->tag = (const char*)in->tag.p;
        out->tag_len = in->tag.len;
        out->implicit = (in->tag.len == 0);
        out->style = in->flow ? YEP_LY_FLOW_STYLE : YEP_LY_BLOCK_STYLE;
        return 0;
    case YEP_EV_SEQ_END:
        out->type = YEP_LY_SEQUENCE_END_EVENT;
        return 0;
    case YEP_EV_MAP_START:
        out->type = YEP_LY_MAPPING_START_EVENT;
        out->anchor = (const char*)in->anchor.p;
        out->anchor_len = in->anchor.len;
        out->tag = (const char*)in->tag.p;
        out->tag_len = in->tag.len;
        out->implicit = (in->tag.len == 0);
        out->style = in->flow ? YEP_LY_FLOW_STYLE : YEP_LY_BLOCK_STYLE;
        return 0;
    case YEP_EV_MAP_END:
        out->type = YEP_LY_MAPPING_END_EVENT;
        return 0;
    default:
        return -1;
    }
}

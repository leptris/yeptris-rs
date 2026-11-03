/* tags.c — the tag identity SSOT (TODO.impl/10).
 *
 * Dense ids for the core set; canonical URIs live here once. Custom
 * tags keep their string (the DOM already stores the view) and use
 * YEPTRIS_TAG_CUSTOM.
 */

#include "resolver.h"

#include <string.h>

typedef struct {
    yep_tag_id id;
    const char* uri;
} yep_tag_entry;

static const yep_tag_entry k_core[] = {
    {0, "tag:yaml.org,2002:str"},    {1, "tag:yaml.org,2002:int"},
    {2, "tag:yaml.org,2002:float"},  {3, "tag:yaml.org,2002:bool"},
    {4, "tag:yaml.org,2002:null"},   {5, "tag:yaml.org,2002:timestamp"},
    {6, "tag:yaml.org,2002:seq"},    {7, "tag:yaml.org,2002:map"},
    {8, "tag:yaml.org,2002:binary"}, {9, "tag:yaml.org,2002:merge"},
    {10, "tag:yaml.org,2002:value"},
};

const char* yep_tag_uri(yep_tag_id id) {
    if (id < sizeof(k_core) / sizeof(k_core[0])) {
        return k_core[id].uri;
    }
    return NULL;
}

yep_tag_id yep_tag_from_uri(const char* p, uint32_t len) {
    for (size_t i = 0; i < sizeof(k_core) / sizeof(k_core[0]); i++) {
        if (strlen(k_core[i].uri) == len && memcmp(k_core[i].uri, p, len) == 0) {
            return k_core[i].id;
        }
    }
    return 11; /* YEPTRIS_TAG_CUSTOM */
}

const char* yep_tag_float_word(const char* p, uint32_t len) {
    static const struct {
        const char* word;
        const char* canon;
    } words[] = {
        {".inf", ".inf"},   {".Inf", ".inf"},   {".INF", ".inf"},  {"-.inf", "-.inf"},
        {"-.Inf", "-.inf"}, {"-.INF", "-.inf"}, {"+.inf", ".inf"}, {"+.Inf", ".inf"},
        {"+.INF", ".inf"},  {".nan", ".nan"},   {".NaN", ".nan"},  {".NAN", ".nan"},
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        uint32_t wl = (uint32_t)strlen(words[i].word);
        if (len == wl && memcmp(p, words[i].word, wl) == 0) {
            return words[i].canon;
        }
    }
    return NULL;
}

/* style.c — the style rule table (TODO.impl/13).
 *
 * The plain-safety predicate LIVES in scan/scan.c (yep_scan_plain_safe,
 * #352 lane 3): bytes→facts is scan/'s alone, and the DOM's parse-time
 * fact bit and this emitter side read the ONE walk so the verdicts
 * cannot drift. These are the emit-side names over it. */

#include "style.h"

#include "../scan/scan.h"

int yep_style_plain_safe(const char* p, uint32_t len) {
    return yep_scan_plain_safe(p, len);
}

int yep_style_plain_key_safe(const char* p, uint32_t len) {
    /* plain_safe's colon rule is the key rule too: a colon only ends a
     * key when followed by a blank/EOL, so "a:b" and ":name" are plain
     * keys (libyaml emits Psych's symbol dumps exactly this way) */
    return yep_scan_plain_safe(p, len);
}

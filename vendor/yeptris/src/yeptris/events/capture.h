/* capture.h — shared event capture (TODO.impl/12).
 *
 * One sink that appends fixed-size records plus copied strings into an
 * arena. Pull, recorder and iterparse are storage policies over this
 * machinery; push converts directly (no storage). The engine knows
 * nothing about any of them (OCP).
 */
#ifndef YEP_EVENTS_CAPTURE_H
#define YEP_EVENTS_CAPTURE_H

#include <stdint.h>
#include <stdlib.h>

#include "../../include/yeptris/events.h"
#include "parse/engine.h"
#include "parse/events.h"

/* Growable records array + string arena; records are the public
 * YeptrisEventRecord layout (the storage SSOT for pull/recorder/
 * iterparse). */
typedef struct {
    YeptrisEventRecord* recs;
    size_t n, cap;
    char* arena;
    size_t arena_len, arena_cap;
} yep_rec_store;

void yep_rec_init(yep_rec_store* s);
void yep_rec_reset(yep_rec_store* s);
void yep_rec_free(yep_rec_store* s);

/* The sink: appends one record per event, strings copied to the arena. */
int yep_rec_on_event(void* ctx, const yep_event* ev);

/* Materializes record i into the public event shape; string pointers
 * reference this store's arena. */
void yep_rec_materialize(const yep_rec_store* s, size_t i, void* out /* YeptrisEvent* */);

#endif /* YEP_EVENTS_CAPTURE_H */

/* yeptris.h — umbrella header for libyeptris.
 *
 * Ultra-fast YAML 1.2 parser, emitter and streamer in C; the YAML
 * counterpart of libleptris. Subsystem headers grow with their items
 * (TODO.impl/01-20); this file only includes the public surface.
 */
#ifndef YEPTRIS_H
#define YEPTRIS_H

#include <yeptris/api.h>
#include <yeptris/dom.h>
#include <yeptris/emit.h>
#include <yeptris/error.h>
#ifdef YEPTRIS_WITH_CBOR
#include <yeptris/cbor.h>
#endif
#include <yeptris/events.h>
#include <yeptris/parse.h>
#include <yeptris/resolve.h>
#include <yeptris/types.h>
#include <yeptris/validate.h>
#include <yeptris/version.h>

#endif /* YEPTRIS_H */

/* parse.c — public API glue: encoding front-end → engine → DOM. */

#include <string.h>

#include "common/simd_text.h"
#include "doc.h"
#include "dom/dom.h"
#include "dom/ytape.h"
#include "encoding/encoding.h"
#include "memory/allocator.h"
#include "memory/pool.h"
#include "parse/engine.h"
#include "parse/numbers.h"
#include "resolve/resolver.h"
#include "scan/json.h"
#include "tape_in.h"

#include "parse/events.h"
#include <errno.h>
#include <math.h>
#include <stdlib.h>

#include <yeptris.h>

YEPTRIS_API const char* yeptris_last_error(uint32_t* line, uint32_t* col) {
    const yep_error* tls = yep_error_tls();
    if (line != NULL) {
        *line = tls->line;
    }
    if (col != NULL) {
        *col = tls->col;
    }
    return tls->msg;
}

static YeptrisDocument parse_impl(const char* buf, size_t len, const YeptrisParseOptions* opts,
                                  int json_mode, int lazy, YeptrisStatus* status);

/* The schema -> resolver dispatch point (OCP: the typing SSOT's ONE
 * switch). Every build route selects its resolver here — the event
 * entry, the engine and the direct-builder DOM, the fused block
 * runner (and validate/stream.c's event route, through the event
 * entry). */
const yep_resolver* resolver_for_schema(YeptrisSchema schema) {
    switch (schema) {
    case YEPTRIS_SCHEMA_11_COMPAT:
        return yep_resolver_compat11();
    case YEPTRIS_SCHEMA_12_JSON:
        return yep_resolver_json12();
    case YEPTRIS_SCHEMA_12_FULL:
        return yep_resolver_full12();
    default:
        return yep_resolver_core12(); /* 12_CORE and the zero default */
    }
}

YEPTRIS_API YeptrisDocument yeptris_parse(const char* buf, size_t len, YeptrisStatus* status) {
    return yeptris_parse_ex(buf, len, NULL, status);
}

/* #342 slice 2: materialize the deferred tree on first access. The
 * gate-clean JSON route carries the parsed TAPE (simdjson's design:
 * its "DOM" is a tape) and pays dom_from_tape only when a consumer
 * touches the tree — parse-only workloads never build nodes. */
yep_dom* yep_doc_dom(yeptris_document* doc) {
    if (doc == NULL) {
        return NULL;
    }
    /* the midx discipline (the #157 TSAN lesson): EVERY dom read and
     * write rides the mutex — the unlock publishes the built tree
     * (readers get their happens-before through the lock), so the
     * plain field needs no atomics. Entry-level only: node walks ride
     * handles created after this returns. */
    yep_mutex_lock(&doc->lazy_mu);
    if (doc->dom == NULL && doc->lazy_tape != NULL) {
        if (doc->lazy_kind == 1) {
            /* #378: the YAML record tape — replay through the eager
             * builders; the finish pool (whose spans the records
             * carry) dies once the builders copied what must outlive
             * it */
            yep_ytape* t = (yep_ytape*)doc->lazy_tape;
            yep_dom* dom = yep_dom_create(doc->sys);
            if (dom != NULL) {
                dom->resolver = resolver_for_schema(doc->schema);
            }
            if (dom != NULL && dom_from_ytape(dom, t) == 0) {
                doc->dom = dom;
                ytap_free(t);
                yep_free(doc->sys, t);
                doc->lazy_tape = NULL;
                yep_pool_destroy((yep_pool*)doc->finish_pool);
                doc->finish_pool = NULL;
            } else {
                yep_dom_destroy(dom);
                /* the tape stays: a later access retries, or free
                 * drops it (replay fails only on allocation/depth, as
                 * the builders do) */
            }
        } else {
            yeptris_json_tape* t = (yeptris_json_tape*)doc->lazy_tape;
            yep_dom* dom = yep_dom_create(doc->sys);
            if (dom != NULL && dom_from_tape(dom, t) == 0) {
                doc->dom = dom;
                yeptris_tape_free(t);
                yep_free(doc->sys, t);
                doc->lazy_tape = NULL;
            } else {
                yep_dom_destroy(dom);
                /* the tape stays: a later access retries, or free
                 * drops it (the materializer only fails on malformed
                 * NUM spans, which the strict gate already rejected at
                 * parse) */
            }
        }
    }
    yep_dom* dom = doc->dom;
    yep_mutex_unlock(&doc->lazy_mu);
    return dom;
}

/* The strict-JSON document wrapper (both routes share it). */
static YeptrisDocument yep_json_doc_wrap(yep_dom* dom, const char* buf, size_t len,
                                         const yep_allocator* sys, YeptrisStatus* status) {
    (void)len;
    yeptris_document* doc = yep_alloc(sys, sizeof(yeptris_document));
    if (doc == NULL) {
        yep_dom_destroy(dom);
        if (status != NULL) {
            *status = YEPTRIS_ERROR_MEMORY;
        }
        return NULL;
    }
    yep_mutex_init(&doc->lazy_mu);
    doc->dom = dom;
    doc->sys = sys;
    doc->lazy_tape = NULL;
    doc->lazy_kind = 0;
    doc->schema = YEPTRIS_SCHEMA_12_CORE; /* strict JSON is core by construction */
    doc->transcoded = NULL;
    doc->transcoded_len = 0;
    doc->input = buf;
    doc->finish_pool = NULL;
    if (status != NULL) {
        *status = YEPTRIS_OK;
    }
    return (YeptrisDocument)doc;
}

YEPTRIS_API YeptrisDocument yeptris_parse_json(const char* buf, size_t len, YeptrisStatus* status) {
    YeptrisStatus st = YEPTRIS_OK;
    if (buf == NULL && len != 0) {
        st = YEPTRIS_ERROR_ARG;
        goto jfail;
    }
    /* Clean-walk fast route (TODO.restructure/81 stage 2, #342): the
     * fused walk runs clean_only — tabs and non-ASCII bytes REJECT —
     * and ANY reject falls to the original sequence below, whose error
     * precedence is byte-for-byte the pinned behavior
     * (json-suite-strict gates this). The entry's full-input gate_scan
     * + tab-memchr pre-passes are GONE: the walk's own byte guarantees
     * route identically by construction, and the strict path re-gates
     * only when actually taken (the profile's 5% + 2% lanes). */
    {
        size_t off = 0;
        while (off < len &&
               (buf[off] == ' ' || buf[off] == '\t' || buf[off] == '\n' || buf[off] == '\r')) {
            off++;
        }
        if (off < len && (buf[off] == '[' || buf[off] == '{')) {
            const yep_allocator* sys = yep_system_allocator();
            yeptris_json_tape* t = yep_alloc(sys, sizeof(*t));
            if (t == NULL) {
                st = YEPTRIS_ERROR_MEMORY;
                goto jfail;
            }
            if (yep_tape_walk_lenient_fused(buf, len, off, t, 1, 1, 0) == YEPTRIS_OK &&
                !yep_tape_long_simple_key(t)) {
                /* strict_nums=1: the arms validated the numbers at
                 * record time — the settle pass is gone (the profile's
                 * 10% lane; the spans were cache-warm in the walk).
                 * The simple-key law: a >1024 flow key cannot
                 * re-serialize — the engine's clean error decides (run
                 * 38143486175) */
                YeptrisDocument h = yep_json_doc_wrap(NULL, buf, len, sys, status);
                if (h != NULL) {
                    ((yeptris_document*)h)->lazy_tape = t;
                    ((yeptris_document*)h)->lazy_kind = 0;
                    return h;
                }
                yeptris_tape_free(t); /* wrap failed (memory): below */
            } else {
                yeptris_tape_free(t); /* reject (incl. clean-route tabs
                                         and non-ASCII): below */
            }
            yep_free(sys, t);
        }
    }
    size_t verr = 0;
    if (!yep_json_document(buf, len, &verr)) {
        yep_error_set(yep_error_tls(), YEP_ERR_UNEXPECTED, 0, 0, verr,
                      "not strict JSON at byte %zu", verr);
        st = YEPTRIS_ERROR_PARSE;
        goto jfail;
    }
    /* the fallback re-gates: a buffer here was walk-rejected, so its
     * printability decides whether UTF-8 validation is owed */
    int gated = len > 0 && yep_text_active()->gate_scan(buf, len);
    size_t uerr = 0;
    if (gated && !yep_utf8_validate((const unsigned char*)buf, len, &uerr)) {
        yep_error_set(yep_error_tls(), YEP_ERR_ENCODING, 0, 0, uerr, "ill-formed UTF-8 at byte %zu",
                      uerr);
        st = YEPTRIS_ERROR_ENCODING;
        goto jfail;
    }
    /* Direct DOM construction (TODO.impl/27): the validated buffer
     * needs no engine — one walk builds the tree, skipping the event
     * pipeline. A builder/validator disagreement defers to the engine
     * (never happens in-tree; belt and braces). */
    {
        const yep_allocator* sys = yep_system_allocator();
        yep_dom* dom = yep_dom_create(sys);
        if (dom == NULL) {
            st = YEPTRIS_ERROR_MEMORY;
            goto jfail;
        }
        dom->input_base = buf; /* strict JSON is UTF-8 by definition */
        dom->input_len = len;
        yep_dom_prepare_len(dom, len);
        int brc = yep_dom_build_json(dom, buf, len);
        if (brc == -1) {
            yep_dom_destroy(dom);
            st = YEPTRIS_ERROR_MEMORY;
            goto jfail;
        }
        if (brc == -2) {
            yep_dom_destroy(dom);
            return parse_impl(buf, len, NULL, 1, 0, status);
        }
        return yep_json_doc_wrap(dom, buf, len, sys, status);
    }
jfail:
    if (status != NULL) {
        *status = st;
    }
    return NULL;
}

/* The shared parse front-end (the encoding law, one home): BOM sniff;
 * borrow UTF-8, transcode the rest; printability; NEL normalization.
 * Returns 0 ok, -1 failed (*st carries ENCODING). *transcoded owns any
 * copy (the system allocator frees it); *data and *data_len point at
 * the bytes the engine runs on. json_mode skips the YAML-only passes (the
 * JSON reader validated its own grammar and well-formedness at the
 * entry and keeps U+0085 raw inside strings). */
static int parse_frontend(const char* buf, size_t len, const yep_allocator* sys, int json_mode,
                          const char** data_out, size_t* data_len_out,
                          unsigned char** transcoded_out, size_t* transcoded_len_out,
                          int* norm_kept_raw_out, YeptrisStatus* st) {
    /* a NULL/0-byte stream is a valid empty input: parse it through a
     * non-null base so the BOM arithmetic never computes NULL+0
     * (clang's UBSan flags the zero-offset-null form; gcc's does not —
     * the schema-econ BadInputs pin surfaced it on the macOS arm64 leg) */
    const char* src = buf != NULL ? buf : "";
    const char* data = src;
    size_t data_len = len;
    unsigned char* transcoded = NULL;
    size_t transcoded_len = 0;
    yep_encoding enc = YEP_ENC_UNKNOWN;
    int norm_kept_raw = 0; /* raw NEL/LS/PS survived the quote-aware
                            * walk: the engine route owns the document
                            * (the tape runner predates the cut) */

    if (json_mode) {
        /* JSON mode validated grammar and UTF-8 well-formedness at the
         * entry: no BOM, no transcoding, and never the printable set —
         * RFC 8259 allows DEL and noncharacters that YAML c-printable
         * excludes. The engine takes the bytes directly. */
        *data_out = data;
        *data_len_out = data_len;
        *transcoded_out = NULL;
        *transcoded_len_out = 0;
        *norm_kept_raw_out = 0;
        return 0;
    }

    size_t bom = yep_bom_sniff((const unsigned char*)src, len, &enc);
    data = src + bom;
    data_len = len - bom;

    if (enc == YEP_ENC_UTF16LE || enc == YEP_ENC_UTF16BE || enc == YEP_ENC_UTF32LE ||
        enc == YEP_ENC_UTF32BE) {
        size_t terr = 0;
        int rc = yep_transcode_to_utf8(sys, enc, (const unsigned char*)data, data_len, &transcoded,
                                       &transcoded_len, &terr);
        if (rc != 0) {
            yep_error_set(yep_error_tls(), YEP_ERR_ENCODING, 0, 0, terr,
                          "ill-formed input encoding");
            yep_free(sys, transcoded);
            *st = YEPTRIS_ERROR_ENCODING;
            return -1;
        }
        data = (const char*)transcoded;
        data_len = transcoded_len;
        size_t pverr = 0;
        if (!yep_printable_validate((const unsigned char*)data, data_len, &pverr)) {
            yep_error_set(yep_error_tls(), YEP_ERR_ENCODING, 0, 0, pverr,
                          "non-printable character at byte %zu", pverr);
            yep_free(sys, transcoded);
            *st = YEPTRIS_ERROR_ENCODING;
            return -1;
        }
    } else {
        /* The encoding gate rides the cheap gate_scan kernel; sizing
         * rides length heuristics (TODO.restructure/66: the fused
         * multi-class stats sweep cost ~12% on the ubuntu asymmetry
         * shapes — ryml pays no pre-pass). Pure gate-safe input skips
         * the SWAR validator entirely; anything else falls to it for
         * the authoritative answer + error position. */
        if (yep_text_active()->gate_scan(data, data_len)) {
            size_t verr = 0;
            if (!yep_printable_validate((const unsigned char*)data, data_len, &verr)) {
                yep_error_set(yep_error_tls(), YEP_ERR_ENCODING, 0, 0, verr,
                              "ill-formed or non-printable UTF-8 at byte %zu", verr);
                *st = YEPTRIS_ERROR_ENCODING;
                return -1;
            }
        }
    }

    /* Unicode breaks (NEL/LS/PS) normalize to '\n' BEFORE the engine:
     * the copy rides `transcoded`, whose ownership plumbing (input_base
     * views, doc free, fail paths) already exists. Letting the engine's
     * per-feed copy fire here returned views into ENGINE-owned memory
     * that dies at yeptris_parse teardown — every one-shot NEL document
     * dangled (no test ever covered one; found by the NormBreakLeads
     * slice, 2026-09-27). JSON mode skips this: the JSON reader keeps
     * U+0085 raw inside strings, only the YAML reader normalizes. */
    {
        size_t nlen = 0;
        char* norm = yep_engine_normalize_breaks(sys, data, data_len, &nlen, &norm_kept_raw);
        if (norm != NULL) {
            yep_free(sys, transcoded);
            transcoded = (unsigned char*)norm;
            transcoded_len = nlen;
            data = norm;
            data_len = nlen;
        }
    }

    *data_out = data;
    *data_len_out = data_len;
    *transcoded_out = transcoded;
    *transcoded_len_out = transcoded_len;
    *norm_kept_raw_out = norm_kept_raw;
    return 0;
}

/* parse.c's sink-entry seam (#902): the DOM route's exact front-end and
 * engine run into a caller sink — no DOM, no tape. Statuses are the
 * parse route's (OK, or PARSE / DEPTH / ENCODING / MEMORY; the sink's
 * own failure rides MEMORY like the DOM builder's). on_input (may be
 * NULL) receives the run's input view — the DOM route's input_base law
 * (the transcoded copy, or the caller's bytes) and its post-BOM length
 * — immediately before the engine runs, because the events' borrowed
 * views subtract from it. The bytes live only for the run. */
YeptrisStatus yep_parse_events(const char* buf, size_t len, YeptrisSchema schema,
                               const yep_sink* sink,
                               void (*on_input)(void* ctx, const char* base, size_t len),
                               void* on_ctx) {
    if (sink == NULL || (buf == NULL && len != 0)) {
        return YEPTRIS_ERROR_ARG;
    }
    const yep_allocator* sys = yep_system_allocator();
    const char* data = NULL;
    size_t data_len = 0;
    unsigned char* transcoded = NULL;
    size_t transcoded_len = 0;
    int norm_kept_raw = 0;
    YeptrisStatus st = YEPTRIS_OK;
    if (parse_frontend(buf, len, sys, 0, &data, &data_len, &transcoded, &transcoded_len,
                       &norm_kept_raw, &st) != 0) {
        return st;
    }
    yep_engine* eng = yep_engine_create(sys);
    if (eng == NULL) {
        yep_free(sys, transcoded);
        return YEPTRIS_ERROR_MEMORY;
    }
    /* the typing SSOT: the family's resolver (resolver_for_schema is
     * the one switch) */
    yep_engine_set_resolver(eng, resolver_for_schema(schema));
    if (schema == YEPTRIS_SCHEMA_11_COMPAT) {
        /* the compat schema is the psych/libyaml surface (see
         * parse_impl's twin comment) */
        yep_engine_set_compat_grammar(eng, 1);
    }
    {
        /* the nametab reserve survives: a memchr chain for '&' is far
         * cheaper than the class sweep it replaced */
        yep_text_stats amp_only = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        const char* q = data == NULL ? "" : data;
        const char* end = q + data_len;
        if (q != NULL) {
            while ((q = memchr(q, '&', (size_t)(end - q))) != NULL) {
                amp_only.amp++;
                q++;
            }
        }
        yep_engine_prepare(eng, &amp_only);
    }
    yep_engine_front_done(eng);
    if (on_input != NULL) {
        on_input(on_ctx, transcoded ? (const char*)transcoded : buf, data_len);
    }
    int rc = yep_engine_run(eng, data, data_len, sink);
    if (rc != 0) {
        /* the error CODE decides the public status — the parse route's
         * law (parse_impl carries the full comment): a CLEAN channel
         * under a failed run means the SINK failed */
        const yep_error* ee = yep_engine_error(eng);
        yep_err_code code = (ee && ee->code != YEP_ERR_NONE) ? ee->code : YEP_ERR_MEMORY;
        if (ee) {
            yep_error* tls = yep_error_tls();
            *tls = *ee;
        }
        yep_pool* finish = yep_engine_detach_pool(eng);
        yep_pool_destroy(finish);
        yep_engine_destroy(eng);
        yep_free(sys, transcoded);
        return (code == YEP_ERR_MEMORY)  ? YEPTRIS_ERROR_MEMORY
               : (code == YEP_ERR_DEPTH) ? YEPTRIS_ERROR_DEPTH
                                         : YEPTRIS_ERROR_PARSE;
    }
    yep_pool* finish = yep_engine_detach_pool(eng);
    yep_pool_destroy(finish); /* nothing borrows the finish pool: every
                               * consumer copies (snapshot law) */
    yep_engine_destroy(eng);
    yep_free(sys, transcoded);
    return YEPTRIS_OK;
}

static YeptrisDocument parse_impl(const char* buf, size_t len, const YeptrisParseOptions* opts,
                                  int json_mode, int lazy, YeptrisStatus* status) {
    YeptrisStatus st = YEPTRIS_OK;
    if (buf == NULL && len != 0) {
        st = YEPTRIS_ERROR_ARG;
        goto fail;
    }

    const yep_allocator* sys = yep_system_allocator();

    /* Encoding front-end (parse_frontend owns the law): BOM sniff;
     * borrow UTF-8, transcode the rest; NEL normalization. */
    const char* data = NULL;
    size_t data_len = 0;
    unsigned char* transcoded = NULL;
    size_t transcoded_len = 0;
    int norm_kept_raw = 0; /* raw NEL/LS/PS survived the quote-aware
                            * walk: the engine route owns the document
                            * (the tape runner predates the cut) */
    char* nbuf = NULL;     /* the engine's break-normalizer copy (#953):
                            * DOM and tape scalar slices point INTO it,
                            * so the document must own it like the
                            * finish pool and transcoded */
    size_t nblen = 0;

    if (parse_frontend(buf, len, sys, json_mode, &data, &data_len, &transcoded, &transcoded_len,
                       &norm_kept_raw, &st) != 0) {
        goto fail;
    }

    /* Engine → DOM. */
    const YeptrisSchema schema = opts != NULL ? opts->schema : YEPTRIS_SCHEMA_12_CORE;
    yep_engine* eng = NULL;
    eng = yep_engine_create(sys);
    if (opts != NULL) {
        /* the typing SSOT: the family's resolver (resolver_for_schema
         * is the one switch) */
        yep_engine_set_resolver(eng, resolver_for_schema(schema));
        if (opts->schema == YEPTRIS_SCHEMA_11_COMPAT) {
            /* the compat schema is the psych/libyaml surface: its grammar
             * must accept what libyaml accepts (the flow floor is suite-
             * strict, not libyaml — see yep_engine_set_compat_grammar) */
            yep_engine_set_compat_grammar(eng, 1);
        }
        if (opts->max_depth > 0) {
            yep_engine_set_max_depth(eng, opts->max_depth);
        }
    }
    if (eng == NULL) {
        yep_free(sys, transcoded);
        st = YEPTRIS_ERROR_MEMORY;
        goto fail;
    }
    /* the lazy route builds NO tree at parse: the dom (and its whole
     * setup) exists only for the eager sink (the ASAN leak lesson —
     * an unused dom leaked per lazy parse) */
    yep_dom* dom = NULL;
    if (!lazy) {
        dom = yep_dom_create(sys);
        if (dom == NULL) {
            yep_engine_destroy(eng);
            yep_free(sys, transcoded);
            st = YEPTRIS_ERROR_MEMORY;
            goto fail;
        }
        /* the direct builders resolve with the engine's schema (the typing
         * SSOT — one resolver decision per scalar, whichever path builds) */
        dom->resolver = resolver_for_schema(schema);
        /* BEFORE the run: input-slice strings borrow as views (zero copy)
         * — the document keeps `transcoded` alive for exactly this. Set
         * after, every scalar was arena-copied (found 2026-09-10). */
        dom->input_len = data_len;
        dom->input_base = transcoded ? (const char*)transcoded : buf;
        yep_dom_prepare_len(dom, data_len);
    }
    if (!json_mode) {
        /* the nametab reserve survives: a memchr chain for '&' is
         * far cheaper than the class sweep it replaced. JSON skips it
         * entirely — no anchors exist (the profile's 4.4% memchr
         * lane on the json corpora). */
        yep_text_stats amp_only = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        /* (NULL, 0) passes the arg guard: NULL + 0 is UB, not an
         * empty range (UBSan, the first nightly under the combined
         * fuzz build) */
        const char* q = data == NULL ? "" : data;
        const char* end = q + data_len;
        if (q != NULL) {
            while ((q = memchr(q, '&', (size_t)(end - q))) != NULL) {
                amp_only.amp++;
                q++;
            }
        }
        yep_engine_prepare(eng, &amp_only);
    }

    yep_ytape yt = {0}; /* zeroed: MSVC cannot couple have_tape to ytap_init's fill */
    int have_tape = 0;
    yep_sink sink;
    if (lazy) {
        /* #378: the recorder rides the SAME engine run — the fused
         * arms stay hot, the builders' node lane defers to replay */
        if (ytap_init(&yt, data, data_len, 0) != 0) {
            yep_engine_destroy(eng);
            yep_free(sys, transcoded);
            st = YEPTRIS_ERROR_MEMORY;
            goto fail;
        }
        ytap_sink(&yt, &sink);
        have_tape = 1;
    } else {
        sink = (yep_sink){.on_event = yep_dom_on_event,
                          .ctx = dom,
                          .on_flow_build = dom_on_flow_build,
                          .on_flow_commit = dom_on_flow_commit,
                          .on_flow_rollback = dom_on_flow_rollback,
                          .on_block_pair = dom_on_block_pair,
                          .on_scalar = dom_on_scalar,
                          .on_block_open = dom_on_block_open,
                          .on_block_item = dom_on_block_item,
                          .compose = 1};
    }
    int rc = 0;
    if (have_tape && !norm_kept_raw && ytap_flow_rooted(&yt) == 0) {
        /* a pure flow-rooted document: the classification ran once and
         * the scaffolding landed — neither runner nor engine runs */
        yep_engine_destroy(eng);
        eng = NULL;
    } else if (have_tape && !norm_kept_raw &&
               ytap_fused_run(&yt, resolver_for_schema(schema)) == 0) {
        /* the fused block runner owned the whole document — the engine
         * never runs (TODO.restructure/79). Its only cost was the parse
         * front-end above; on a bail it unwound and the engine runs. */
        yep_engine_destroy(eng);
        eng = NULL;
    } else {
        if (have_tape) {
            ytap_reset(&yt);
        }
        yep_engine_front_done(eng);
        rc = yep_engine_run(eng, data, data_len, &sink);
    }
    if (rc != 0) {
        const yep_error* ee = yep_engine_error(eng);
        /* a CLEAN channel under a failed run means the SINK failed — a
         * builder ran out of memory (the DOM's oom law), the bytes
         * never were a parse error. Mapping it to PARSE let
         * yep_dom_str_put's swallowed OOM surface as a syntax error
         * (the audit's finding: silent doc corruption reads worse). */
        yep_err_code code = (ee && ee->code != YEP_ERR_NONE) ? ee->code : YEP_ERR_MEMORY;
        if (ee) {
            yep_error* tls = yep_error_tls();
            *tls = *ee;
        }
        if (have_tape) {
            ytap_free(&yt);
        }
        yep_dom_destroy(dom);
        yep_engine_destroy(eng);
        yep_free(sys, transcoded);
        st = (code == YEP_ERR_MEMORY)  ? YEPTRIS_ERROR_MEMORY
             : (code == YEP_ERR_DEPTH) ? YEPTRIS_ERROR_DEPTH
                                       : YEPTRIS_ERROR_PARSE;
        goto fail;
    }
    /* Resolved tags, folded and escaped scalars live in the engine's
     * finish pool; the document must own it or every such string would
     * dangle at engine teardown (found by ASAN). The break-normalizer
     * copy is the same law (#953): scalar value slices in the DOM and
     * the packed tape point INTO it. */
    yep_pool* finish = yep_engine_detach_pool(eng);
    nbuf = yep_engine_detach_norm_buf(eng, &nblen);
    yep_engine_destroy(eng);

    /* Empty stream (no documents): NULL document with YEPTRIS_OK. */
    if (have_tape ? (yt.docs == 0) : (dom->dcount == 0)) {
        if (have_tape) {
            ytap_free(&yt);
        } else {
            yep_dom_destroy(dom);
        }
        yep_pool_destroy(finish);
        yep_free(sys, transcoded);
        if (nbuf != NULL) {
            yep_free(sys, nbuf); /* not parked: no document survives */
        }
        if (status != NULL) {
            *status = YEPTRIS_OK;
        }
        return NULL;
    }
    if (!have_tape) {
        /* DOM strings are input-offsets or arena copies; nothing in the
         * tree references the finish pool anymore — release it now */
        yep_pool_destroy(finish);
        finish = NULL;
    } /* the lazy route keeps it: the records carry pool spans until
         replay copies (doc.h's finish_pool contract) */

    yeptris_document* doc = yep_alloc(sys, sizeof(yeptris_document));
    if (doc == NULL) {
        yep_dom_destroy(dom);
        yep_pool_destroy(finish);
        yep_free(sys, transcoded);
        if (nbuf != NULL) {
            yep_free(sys, nbuf); /* not parked: no document survives */
        }
        st = YEPTRIS_ERROR_MEMORY;
        goto fail;
    }
    yep_mutex_init(&doc->lazy_mu);
    doc->sys = sys;
    doc->schema = (opts != NULL && opts->schema != YEPTRIS_SCHEMA_12_CORE) ? opts->schema
                                                                           : YEPTRIS_SCHEMA_12_CORE;
    doc->transcoded = transcoded;
    doc->transcoded_len = transcoded_len;
    if (nbuf != NULL) {
        /* #953: the engine re-normalized THIS run (raw NEL/LS/PS rode
         * quoted regions past the front-end), so every engine event
         * view points INTO nbuf — and the lazy tape stored those
         * pointers ABSOLUTE (they fall outside yt.input's range, so
         * the span recorder kept them verbatim). The document owns
         * nbuf like the finish pool, whichever copy the slot held. */
        if (have_tape) {
            /* rebase the tape's span base onto the doc-owned copy:
             * materialization's input_base offsets against LIVE bytes.
             * `transcoded` is referenced by no record (the engine
             * parsed nbuf only) — release it. */
            yt.input = nbuf;
            yt.input_len = nblen;
            yep_free(sys, transcoded);
            transcoded = NULL;
            doc->transcoded = (unsigned char*)nbuf;
            doc->transcoded_len = nblen;
        } else {
            /* eager: the dom copied every scalar during the run (the
             * borrowed views offset against input_base = transcoded);
             * nbuf has no post-run reader — the slot keeps transcoded */
            yep_free(sys, nbuf);
        }
    }
    doc->input = buf;
    doc->finish_pool = finish; /* NULL on the eager route */
    doc->lazy_tape = NULL;
    doc->lazy_kind = 0;
    if (have_tape) {
        yep_ytape* heap_tape = yep_alloc(sys, sizeof(*heap_tape));
        if (heap_tape == NULL) {
            ytap_free(&yt);
            yep_pool_destroy(finish);
            yep_free(sys, transcoded);
            if (nbuf != NULL) {
                yep_free(sys, nbuf); /* not parked: no document survives */
            }
            yep_free(sys, doc);
            st = YEPTRIS_ERROR_MEMORY;
            goto fail;
        }
        *heap_tape = yt;
        if (yt.pool != NULL && doc->finish_pool == NULL) {
            doc->finish_pool = yt.pool; /* fused block-scalar content: the
                                         * document owns it until replay copies */
        }
        doc->dom = NULL;
        doc->lazy_tape = heap_tape;
        doc->lazy_kind = 1;
    } else {
        doc->dom = dom;
    }
    return (YeptrisDocument)doc;

fail:
    if (status != NULL) {
        *status = st;
    }
    return NULL;
}

YEPTRIS_API YeptrisDocument yeptris_parse_ex(const char* buf, size_t len,
                                             const YeptrisParseOptions* opts,
                                             YeptrisStatus* status) {
    /* #378 slice 2: the default YAML route carries the packed record
     * tape — the tree materializes on first access (yep_doc_dom).
     * Same grammar, same errors; parse-only workloads never build
     * nodes. The eager form stays on yeptris_parse_eager_ex. */
    return parse_impl(buf, len, opts, 0, 1, status);
}

/* #378 slice 1 (internal seam): the YAML parse rides the packed record
 * tape — the tree materializes from the records on first access. Same
 * grammar, same errors; parse-only workloads never build nodes. */
YeptrisDocument yeptris_parse_ytape_ex(const char* buf, size_t len, const YeptrisParseOptions* opts,
                                       YeptrisStatus* status) {
    return parse_impl(buf, len, opts, 0, 1, status);
}

/* The eager form, kept for the parse-only A/B (bench_matrix's #378
 * table) and as the reference lane the differential gates ride. */
YeptrisDocument yeptris_parse_eager_ex(const char* buf, size_t len, const YeptrisParseOptions* opts,
                                       YeptrisStatus* status) {
    return parse_impl(buf, len, opts, 0, 0, status);
}

YEPTRIS_API void yeptris_document_free(YeptrisDocument handle) {
    yeptris_document* doc = (yeptris_document*)handle;
    if (doc == NULL) {
        return;
    }
    if (doc->lazy_tape != NULL) { /* never materialized: free stays free */
        if (doc->lazy_kind == 1) {
            ytap_free((yep_ytape*)doc->lazy_tape);
        } else {
            yeptris_tape_free((yeptris_json_tape*)doc->lazy_tape);
        }
        yep_free(doc->sys, doc->lazy_tape);
    }
    yep_mutex_destroy(&doc->lazy_mu);
    yep_dom_destroy(doc->dom);
    yep_pool_destroy((yep_pool*)doc->finish_pool);
    yep_free(doc->sys, doc->transcoded);
    yep_free(doc->sys, doc);
}

YEPTRIS_API size_t yeptris_document_count(YeptrisDocument handle) {
    yeptris_document* doc = (yeptris_document*)handle;
    yep_dom* dom = doc ? yep_doc_dom(doc) : NULL; /* NULL: the lazy
                                                   * route's failed/retrying materialization — an
                                                   * unusable tree counts as zero documents */
    return dom ? dom->dcount : 0;
}

yeptris_node* yep_handle_new(yeptris_document* doc, uint32_t id) {
    yeptris_node* n = yep_hpool_alloc(yep_dom_handles(doc->dom), sizeof(yeptris_node), 16);
    if (n == NULL) {
        return NULL;
    }
    n->doc = doc;
    n->id = id;
    return n;
}

static YeptrisNode node_new_handle(yeptris_document* doc, uint32_t id) {
    return (YeptrisNode)yep_handle_new(doc, id);
}

YEPTRIS_API YeptrisNode yeptris_document_root(YeptrisDocument handle, size_t index) {
    yeptris_document* doc = (yeptris_document*)handle;
    if (doc == NULL) {
        return NULL;
    }
    yep_dom* dom = yep_doc_dom(doc);
    if (dom == NULL || index >= dom->dcount) {
        return NULL;
    }
    return node_new_handle(doc, dom->docs[index]);
}

static const yep_dnode* node_of(YeptrisNode handle) {
    yeptris_node* n = (yeptris_node*)handle;
    if (n == NULL || n->doc == NULL) {
        return NULL;
    }
    return yep_dom_node(n->doc->dom, n->id);
}

YEPTRIS_API uint32_t yeptris_node_id(YeptrisNode handle) {
    yeptris_node* n = (yeptris_node*)handle;
    return n == NULL ? UINT32_MAX : n->id;
}

YEPTRIS_API YeptrisNodeKind yeptris_node_kind(YeptrisNode handle) {
    const yep_dnode* n = node_of(handle);
    if (n == NULL) {
        return YEPTRIS_NODE_SCALAR;
    }
    switch (n->kind) {
    case YEP_DOM_SEQUENCE:
        return YEPTRIS_NODE_SEQUENCE;
    case YEP_DOM_MAPPING:
        return YEPTRIS_NODE_MAPPING;
    case YEP_DOM_ALIAS:
        return YEPTRIS_NODE_ALIAS;
    default:
        return YEPTRIS_NODE_SCALAR;
    }
}

/* Decodes a node's compact string through its document's regions. */
static yep_view node_view(const yeptris_node* h, yep_sview sv) {
    return yep_dom_view(h->doc->dom, sv);
}

static const char* view_out(yep_view v, size_t* len) {
    if (len != NULL) {
        *len = v.len;
    }
    return v.len ? v.p : (v.p ? v.p : "");
}

YEPTRIS_API const char* yeptris_node_value(YeptrisNode handle, size_t* len) {
    const yep_dnode* n = node_of(handle);
    if (n == NULL || (n->kind != YEP_DOM_SCALAR && n->kind != YEP_DOM_ALIAS)) {
        if (len != NULL) {
            *len = 0;
        }
        return NULL;
    }
    return view_out(node_view((yeptris_node*)handle, n->value), len);
}

YEPTRIS_API YeptrisScalarStyle yeptris_node_style(YeptrisNode handle) {
    const yep_dnode* n = node_of(handle);
    return n ? (YeptrisScalarStyle)n->style : YEPTRIS_STYLE_ANY;
}

YEPTRIS_API const char* yeptris_tag_uri(YeptrisTagId id) {
    return yep_tag_uri((yep_tag_id)id);
}

YEPTRIS_API YeptrisTagId yeptris_node_tag_id(YeptrisNode handle) {
    const yep_dnode* n = node_of(handle);
    if (n == NULL) {
        return YEPTRIS_TAG_CUSTOM;
    }
    /* the INTERNAL kind space here (#904 item 6): n->kind is the
     * dom's yep_dom_kind — the public YEPTRIS_NODE_* constants ride
     * the same values today only by coincidence, and the boundary
     * conversion is yeptris_node_kind's switch alone */
    if (n->kind == YEP_DOM_SEQUENCE) {
        return YEPTRIS_TAG_SEQ;
    }
    if (n->kind == YEP_DOM_MAPPING) {
        return YEPTRIS_TAG_MAP;
    }
    return (YeptrisTagId)n->tag_id;
}

/* strips '_' and ',' into dst (nul-terminated); returns length.
 * The COMMON case (no separators) returns the borrowed view — no
 * copy, no nul-termination needed by the in-place parsers. */
YEPTRIS_API YeptrisStatus yeptris_node_int(YeptrisNode handle, int64_t* out) {
    /* conversion is the number kernel's alone (MECE: one fold, one
     * fast path, one Psych-sexagesimal weight table) */
    const yep_dnode* n = node_of(handle);
    if (n == NULL || out == NULL) {
        return YEPTRIS_ERROR_ARG;
    }
    if (n->tag_id != YEPTRIS_TAG_INT) {
        return YEPTRIS_ERROR_PARSE;
    }
    yep_view v = node_view((yeptris_node*)handle, n->value);
    return yep_num_i64((const char*)v.p, v.len, out) == 0 ? YEPTRIS_OK : YEPTRIS_ERROR_PARSE;
}

YEPTRIS_API YeptrisStatus yeptris_node_float(YeptrisNode handle, double* out) {
    const yep_dnode* n = node_of(handle);
    if (n == NULL || out == NULL) {
        return YEPTRIS_ERROR_ARG;
    }
    if (n->tag_id != YEPTRIS_TAG_FLOAT) {
        return YEPTRIS_ERROR_PARSE;
    }
    yep_view v = node_view((yeptris_node*)handle, n->value);
    return yep_num_f64((const char*)v.p, v.len, out) == 0 ? YEPTRIS_OK : YEPTRIS_ERROR_PARSE;
}
YEPTRIS_API YeptrisStatus yeptris_node_bool(YeptrisNode handle, int* out) {
    const yep_dnode* n = node_of(handle);
    if (n == NULL || out == NULL) {
        return YEPTRIS_ERROR_ARG;
    }
    if (n->tag_id != YEPTRIS_TAG_BOOL) {
        return YEPTRIS_ERROR_PARSE;
    }
    /* the resolver accepted it; true-set membership decides the value */
    static const char* k_true[] = {"true", "True", "TRUE", "y",  "Y", "yes",
                                   "Yes",  "YES",  "on",   "On", "ON"};
    for (size_t i = 0; i < sizeof(k_true) / sizeof(k_true[0]); i++) {
        size_t m = strlen(k_true[i]);
        yep_view v = node_view((yeptris_node*)handle, n->value);
        if (v.len == m && memcmp(v.p, k_true[i], m) == 0) {
            *out = 1;
            return YEPTRIS_OK;
        }
    }
    *out = 0;
    return YEPTRIS_OK;
}

YEPTRIS_API const char* yeptris_node_tag(YeptrisNode handle, size_t* len) {
    const yep_dnode* n = node_of(handle);
    if (n == NULL || n->tag.len == 0) {
        if (len != NULL) {
            *len = 0;
        }
        return n ? NULL : NULL;
    }
    return view_out(node_view((yeptris_node*)handle, n->tag), len);
}

YEPTRIS_API const char* yeptris_node_anchor(YeptrisNode handle, size_t* len) {
    const yep_dnode* n = node_of(handle);
    if (n == NULL || n->anchor.len == 0) {
        if (len != NULL) {
            *len = 0;
        }
        return NULL;
    }
    return view_out(node_view((yeptris_node*)handle, n->anchor), len);
}

static YeptrisNode wrap(yeptris_node* base, uint32_t id) {
    if (base == NULL || base->doc == NULL) {
        return NULL;
    }
    /* Handles live in the document's thread-safe arena: one
     * document_free reclaims every handle ever handed out (zero-leak
     * contract) while read-only sharing stays safe (TODO.impl/19) */
    yeptris_node* n = yep_hpool_alloc(yep_dom_handles(base->doc->dom), sizeof(yeptris_node), 16);
    if (n == NULL) {
        return NULL;
    }
    n->doc = base->doc;
    n->id = id;
    return (YeptrisNode)n;
}

YEPTRIS_API YeptrisNode yeptris_node_alias_target(YeptrisNode handle) {
    const yep_dnode* n = node_of(handle);
    if (n == NULL || n->kind != YEP_DOM_ALIAS || n->target == UINT32_MAX) {
        return NULL;
    }
    return wrap((yeptris_node*)handle, n->target);
}

YEPTRIS_API size_t yeptris_node_seq_count(YeptrisNode handle) {
    const yep_dnode* n = node_of(handle);
    return (n && n->kind == YEP_DOM_SEQUENCE) ? n->count : 0;
}

YEPTRIS_API YeptrisNode yeptris_node_seq_at(YeptrisNode handle, size_t index) {
    const yep_dnode* n = node_of(handle);
    if (n == NULL || n->kind != YEP_DOM_SEQUENCE || index >= n->count) {
        return NULL;
    }
    yep_dom* dom = ((yeptris_node*)handle)->doc->dom;
    uint32_t count = 0;
    /* the node id IS its dnode index */
    uint32_t id = dom_indexed_child(dom, ((yeptris_node*)handle)->id, index, &count); /* #377 */
    if (id == UINT32_MAX) {
        return NULL;
    }
    return wrap((yeptris_node*)handle, id);
}

YEPTRIS_API size_t yeptris_node_children(YeptrisNode handle, YeptrisNode* out, size_t cap) {
    const yep_dnode* n = node_of(handle);
    if (n == NULL || (n->kind != YEP_DOM_SEQUENCE && n->kind != YEP_DOM_MAPPING)) {
        return 0;
    }
    size_t count = 0;
    uint32_t id = n->first_child;
    while (id != UINT32_MAX) {
        if (out != NULL && count < cap) {
            out[count] = wrap((yeptris_node*)handle, id);
        }
        count++;
        const yep_dnode* cur = yep_dom_node(((yeptris_node*)handle)->doc->dom, id);
        id = cur ? cur->next_sibling : UINT32_MAX;
    }
    return count;
}

YEPTRIS_API size_t yeptris_node_map_count(YeptrisNode handle) {
    const yep_dnode* n = node_of(handle);
    return (n && n->kind == YEP_DOM_MAPPING) ? n->count / 2 : 0;
}

YEPTRIS_API int yeptris_node_map_at(YeptrisNode handle, size_t index, YeptrisNode* key,
                                    YeptrisNode* value) {
    const yep_dnode* n = node_of(handle);
    /* the internal kind space (#904 item 6, see yeptris_node_tag_id) */
    if (n == NULL || n->kind != YEP_DOM_MAPPING || index >= n->count / 2) {
        return -1;
    }
    yeptris_node* h = (yeptris_node*)handle;
    yep_dom* d = h->doc->dom;
    uint32_t count = 0;
    uint32_t child = dom_indexed_child(d, h->id, (size_t)index * 2, &count); /* #377 */
    if (child == UINT32_MAX) {
        return -1;
    }
    if (key != NULL) {
        *key = wrap(h, child);
    }
    if (value != NULL) {
        *value = wrap(h, d->nodes[child].next_sibling);
    }
    return 0;
}

YEPTRIS_API YeptrisNode yeptris_node_map_get(YeptrisNode handle, const char* key, size_t key_len) {
    const yep_dnode* n = node_of(handle);
    if (n == NULL || n->kind != YEP_DOM_MAPPING || key == NULL) {
        return NULL;
    }
    yep_dom* dom = ((yeptris_node*)handle)->doc->dom;
    /* O(1) through the lazy index (first use builds it); the scan
     * below is the build-failure fallback */
    uint32_t hit =
        yep_midx_lookup(dom, ((yeptris_node*)handle)->id, (yep_view){key, (uint32_t)key_len});
    if (hit != UINT32_MAX) {
        const yep_dnode* kn = yep_dom_node(dom, hit);
        if (kn != NULL) {
            return wrap((yeptris_node*)handle, kn->next_sibling);
        }
    }
    uint32_t id = n->first_child;
    int want_key = 1;
    while (id != UINT32_MAX) {
        const yep_dnode* cur = yep_dom_node(dom, id);
        if (cur == NULL) {
            break;
        }
        if (want_key && cur->kind == YEP_DOM_SCALAR) {
            yep_view kv = {key, (uint32_t)key_len};
            if (yep_view_eq(yep_dom_view(dom, cur->value), kv)) {
                return wrap((yeptris_node*)handle, cur->next_sibling);
            }
        }
        want_key = !want_key;
        id = cur->next_sibling;
    }
    return NULL;
}

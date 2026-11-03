/* output.c — the structured output face (#906 slice 5).
 *
 * The report -> the json-schema.org RECOMMENDED OUTPUT shape (core
 * spec section 12; meta-schema
 * https://json-schema.org/draft/2020-12/output/schema, fetched into
 * test/conformance/jsonschema/output/schema.json). The published
 * BASIC form is the truthful one for this report: one root output
 * unit carrying "valid", "keywordLocation", "instanceLocation" and —
 * when the report has diagnostics — "errors", each an output unit of
 * its own. The DETAILED/VERBOSE forms are schema-structured
 * hierarchies; the flat report (the enumerated-misses law) carries no
 * schema-side path to build them from, so they are deliberately not
 * faked.
 *
 * keywordLocation is derived from the diagnostic code: the keyword's
 * name segment ("/type", "/required", ...). It is a JSON Pointer but
 * NOT necessarily resolvable in the schema (no schema path in the
 * report); family/bookkeeping codes sit at the root pointer "". $ref
 * reports the root pointer too, so absoluteKeywordLocation — which
 * the meta-schema demands only for /$ref/ locations — is never
 * emitted and the output always satisfies the published meta-schema.
 *
 * Pure and deterministic: report in, bytes out — one measure pass
 * (buf == NULL at the caller's choice) and one write pass compose the
 * same bytes, no allocation on either path, no state. MECE: the
 * diagnostic machinery stays validate.c's; this TU only serializes
 * finished diagnostics.
 */

#include <stdint.h>
#include <string.h>

#include <yeptris/validate.h>

#include "internal.h"

/* The keyword-name table (internal.h's output SSOT): index = the
 * YeptrisDiagCode value, appending with the enum. "" = no JSON Schema
 * keyword (the family/bookkeeping codes, and $ref sites — see the
 * file comment). */
static const char* const keyword_names[] = {
    "",                       /* NONE */
    "",                       /* TAG_MISMATCH (family walk) */
    "",                       /* KEY_NOT_STRING (family walk) */
    "",                       /* NON_JSON_TYPE (family walk) */
    "",                       /* DUPLICATE_KEY (family walk) */
    "/type",                  /* TYPE */
    "/required",              /* REQUIRED */
    "/enum",                  /* ENUM */
    "/const",                 /* CONST */
    "/pattern",               /* PATTERN */
    "/minimum",               /* MINIMUM */
    "/maximum",               /* MAXIMUM */
    "/exclusiveMinimum",      /* EXCLUSIVE_MINIMUM */
    "/exclusiveMaximum",      /* EXCLUSIVE_MAXIMUM */
    "/minLength",             /* MIN_LENGTH */
    "/maxLength",             /* MAX_LENGTH */
    "/minItems",              /* MIN_ITEMS */
    "/maxItems",              /* MAX_ITEMS */
    "/uniqueItems",           /* UNIQUE_ITEMS */
    "/additionalProperties",  /* ADDITIONAL_PROPERTY */
    "/anyOf",                 /* ANY_OF */
    "/oneOf",                 /* ONE_OF */
    "/not",                   /* NOT */
    "",                       /* REF */
    "",                       /* FALSE_SCHEMA */
    "",                       /* DEPTH */
    "",                       /* LIMIT */
    "/multipleOf",            /* MULTIPLE_OF */
    "/minProperties",         /* MIN_PROPERTIES */
    "/maxProperties",         /* MAX_PROPERTIES */
    "/contains",              /* CONTAINS */
    "/minContains",           /* MIN_CONTAINS */
    "/maxContains",           /* MAX_CONTAINS */
    "/dependentRequired",     /* DEPENDENT_REQUIRED */
    "/unevaluatedProperties", /* UNEVALUATED_PROPERTY */
    "/unevaluatedItems",      /* UNEVALUATED_ITEM */
};
/* Appending a diag code without its keyword row is a build break (the
 * output SSOT stays in step with the enum). */
YEPTRIS_CT_ASSERT(sizeof(keyword_names) / sizeof(keyword_names[0]) ==
                  (size_t)YEPTRIS_DIAG_UNEVALUATED_ITEM + 1);

const char* yep_validate_output_keyword(uint32_t code) {
    if (code >= sizeof(keyword_names) / sizeof(keyword_names[0])) {
        return "";
    }
    return keyword_names[code];
}

/* Bounded NUL-terminated length (the report's fields are fixed slots). */
static size_t slot_len(const char* s, size_t cap) {
    size_t n = 0;
    while (n < cap && s[n] != '\0') {
        n++;
    }
    return n;
}

/* RFC 8259 string escaping: quote, backslash and the C0 controls; the
 * five named escapes where JSON has them, \u00XX otherwise. Bytes
 * above 0x1F pass through — the report's text is UTF-8 (the parse
 * validated it). */
static size_t escaped_len(const char* s, size_t n) {
    size_t t = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char b = (unsigned char)s[i];
        switch (b) {
        case '"':
        case '\\':
        case '\b':
        case '\f':
        case '\n':
        case '\r':
        case '\t':
            t += 2;
            break;
        default:
            t += b < 0x20 ? 6 : 1;
            break;
        }
    }
    return t;
}

static void put_lit(char** p, const char* s, size_t n) {
    memcpy(*p, s, n);
    *p += n;
}

static void put_escaped(char** p, const char* s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    char* o = *p;
    for (size_t i = 0; i < n; i++) {
        unsigned char b = (unsigned char)s[i];
        switch (b) {
        case '"':
            *o++ = '\\';
            *o++ = '"';
            break;
        case '\\':
            *o++ = '\\';
            *o++ = '\\';
            break;
        case '\b':
            *o++ = '\\';
            *o++ = 'b';
            break;
        case '\f':
            *o++ = '\\';
            *o++ = 'f';
            break;
        case '\n':
            *o++ = '\\';
            *o++ = 'n';
            break;
        case '\r':
            *o++ = '\\';
            *o++ = 'r';
            break;
        case '\t':
            *o++ = '\\';
            *o++ = 't';
            break;
        default:
            if (b < 0x20) {
                *o++ = '\\';
                *o++ = 'u';
                *o++ = '0';
                *o++ = '0';
                *o++ = hex[b >> 4];
                *o++ = hex[b & 0xF];
            } else {
                *o++ = (char)b;
            }
            break;
        }
    }
    *p = o;
}

#define OUT_ROOT_OPEN                                                                              \
    "{\"valid\":false,\"keywordLocation\":\"\",\"instanceLocation\":\"\",\"errors\":["
#define OUT_ROOT_VALID "{\"valid\":true,\"keywordLocation\":\"\",\"instanceLocation\":\"\"}"
#define OUT_UNIT_OPEN "{\"valid\":false,\"keywordLocation\":\""
#define OUT_INSTANCE "\",\"instanceLocation\":\""
#define OUT_ERROR "\",\"error\":\""
#define OUT_UNIT_CLOSE "\"}"
#define OUT_ROOT_CLOSE "]}"

/* One unit's exact byte cost: open + keyword + path + error + detail +
 * close. */
static size_t unit_len(const yeptris_diagnostic* d) {
    const char* kw = yep_validate_output_keyword(d->code);
    size_t plen = slot_len(d->path, sizeof(d->path));
    size_t dlen = slot_len(d->detail, sizeof(d->detail));
    return (sizeof(OUT_UNIT_OPEN) - 1) + strlen(kw) + (sizeof(OUT_INSTANCE) - 1) +
           escaped_len(d->path, plen) + (sizeof(OUT_ERROR) - 1) + escaped_len(d->detail, dlen) +
           (sizeof(OUT_UNIT_CLOSE) - 1);
}

YEPTRIS_API size_t yeptris_validate_report_write(const yeptris_validate_report* report, char* buf,
                                                 size_t cap) {
    if (report == NULL) {
        return 0;
    }
    size_t need;
    if (report->count == 0) {
        need = sizeof(OUT_ROOT_VALID) - 1;
    } else {
        need = (sizeof(OUT_ROOT_OPEN) - 1) + (sizeof(OUT_ROOT_CLOSE) - 1);
        for (uint32_t i = 0; i < report->count; i++) {
            need += unit_len(&report->diags[i]);
            if (i > 0) {
                need += 1; /* the comma between units */
            }
        }
    }
    if (buf == NULL || cap < need + 1) {
        return need; /* the measure-only form: nothing is written */
    }

    char* p = buf;
    if (report->count == 0) {
        put_lit(&p, OUT_ROOT_VALID, sizeof(OUT_ROOT_VALID) - 1);
    } else {
        put_lit(&p, OUT_ROOT_OPEN, sizeof(OUT_ROOT_OPEN) - 1);
        for (uint32_t i = 0; i < report->count; i++) {
            const yeptris_diagnostic* d = &report->diags[i];
            const char* kw = yep_validate_output_keyword(d->code);
            if (i > 0) {
                *p++ = ',';
            }
            put_lit(&p, OUT_UNIT_OPEN, sizeof(OUT_UNIT_OPEN) - 1);
            put_lit(&p, kw, strlen(kw));
            put_lit(&p, OUT_INSTANCE, sizeof(OUT_INSTANCE) - 1);
            put_escaped(&p, d->path, slot_len(d->path, sizeof(d->path)));
            put_lit(&p, OUT_ERROR, sizeof(OUT_ERROR) - 1);
            put_escaped(&p, d->detail, slot_len(d->detail, sizeof(d->detail)));
            put_lit(&p, OUT_UNIT_CLOSE, sizeof(OUT_UNIT_CLOSE) - 1);
        }
        put_lit(&p, OUT_ROOT_CLOSE, sizeof(OUT_ROOT_CLOSE) - 1);
    }
    *p = '\0';
    return need;
}

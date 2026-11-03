/* scan.h — structure facts from bytes (TODO.impl/06).
 *
 * MECE law: scan answers "where does this line's content start and where
 * do spans end" — never grammar decisions, never allocation. The engine
 * (07) owns grammar. Line facts are produced lazily, one line at a time;
 * no whole-document table (streaming-friendly).
 */
#ifndef YEP_SCAN_H
#define YEP_SCAN_H

#include <stddef.h>
#include <stdint.h>

#include "common/simd_text.h" /* yep_stopset (the stop-class form) */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct yep_line_info {
    uint32_t offset; /* byte offset of the line start */
    uint32_t end;    /* byte offset of the line break (exclusive content end) */
    uint32_t indent; /* column of the first non-space (== end - offset when blank);
                      * 32-bit: a 65536-space line wrapped to 0 and read as a
                      * dedent (the 2026-10-31 audit) */
    uint16_t flags;
    unsigned char first; /* first content byte ('\0' for blank lines) */
} yep_line_info;

enum {
    YEP_LF_BLANK = 1u << 0,     /* only spaces */
    YEP_LF_COMMENT = 1u << 1,   /* first content char is '#' */
    YEP_LF_DOC_START = 1u << 2, /* "---" (then EOL / space / comment) */
    YEP_LF_DOC_END = 1u << 3,   /* "..." (then EOL / space / comment) */
    YEP_LF_DIRECTIVE = 1u << 4, /* '%' at column 0 */
    YEP_LF_TAB = 1u << 5,       /* tab within the indentation (error) */
};

/* Facts for the line starting at pos. Handles \n, \r\n, lone \r breaks. */
yep_line_info yep_scan_line(const char* p, size_t len, size_t pos);

/* What terminated a plain-scalar span. */
typedef enum {
    YEP_TERM_EOF = 0,
    YEP_TERM_EOL,
    YEP_TERM_COMMENT,  /* " #" — span excludes the preceding space */
    YEP_TERM_COLON,    /* ':' followed by blank/EOL — span ends before ':' */
    YEP_TERM_FLOW,     /* flow indicator (flow context only) */
    YEP_TERM_BAD_COLON /* ':' in a position YAML forbids (error context) */
} yep_span_term;

typedef struct yep_span {
    uint32_t start;
    uint32_t end; /* trimmed span end (exclusive) */
    yep_span_term term;
} yep_span;

/* Line-shape classification (TODO.restructure/49): byte-class FACTS for
 * the engine's one-per-line dispatch decision — scan walks, the engine
 * decides. A shape is only produced for the dominant block line forms;
 * every other line classifies NONE and keeps the general paths. */
typedef enum {
    YEP_LSHAPE_NONE = 0, /* not classified: use the general chain */
    YEP_LSHAPE_DASH,     /* "- " entry at the content start */
    YEP_LSHAPE_KEY,      /* plain key + terminating ':' */
} yep_line_kind;

typedef enum {
    YEP_LVAL_NONE = 0,     /* value not classified (bail) */
    YEP_LVAL_EMPTY,        /* EOL or comment after the ':' / '-' */
    YEP_LVAL_PLAIN,        /* plain scalar (val_span + term) */
    YEP_LVAL_ALIAS,        /* '*name' at val_start */
    YEP_LVAL_ANCHOR_PLAIN, /* '&name' then a plain scalar */
    YEP_LVAL_FLOW,         /* '[' / '{' at val_start: the flow kernel owns it */
} yep_line_val;

typedef struct yep_line_shape {
    yep_line_kind kind;
    yep_line_val val;
    uint32_t dash;      /* '-' offset (DASH) */
    uint32_t key_start; /* trimmed plain key span (KEY) */
    uint32_t key_end;
    uint32_t colon;      /* the terminating ':' offset (KEY) */
    uint32_t val_start;  /* first value byte (all non-EMPTY vals) */
    uint32_t anchor_end; /* ANCHOR_PLAIN: end of the anchor name */
    yep_span val_span;   /* PLAIN / ANCHOR_PLAIN: the scalar span + term */
} yep_line_shape;

/* Classifies the content line described by li (its own line; flags must
 * be clear). Zero-initializes *out, then fills the facts. */
void yep_scan_shape(const char* p, size_t len, const yep_line_info* li, yep_line_shape* out);
void yep_scan_shape_f(const char* p, size_t len, const yep_line_info* li, const yep_line_facts* f,
                      yep_line_shape* out);
void yep_scan_line_f(const char* p, size_t len, size_t pos, const yep_line_facts* f,
                     yep_line_info* out);

/* ns-anchor-name byte: not blank/break/flow-indicator, not ',' or '#'. */
int yep_scan_prop_char(unsigned char c);

/* First offset at/after pos that cannot continue an anchor/alias name. */
size_t yep_scan_prop_end(const char* p, size_t len, size_t pos);

/* The dq-forced byte set (issue #352 lane 2): NEL, LS/PS, the BOM,
 * and the noncharacters — the bytes whose only stable spelling is
 * the double-quoted escape. One home for the emitter's route check
 * AND the DOM's parse-time fact bit, so the two cannot drift. */
int yep_scan_dq_forced(const char* p, uint32_t len);

/* The plain-safety truth table (issue #352 lane 3): 1 when the span
 * re-parses as a plain scalar in BLOCK context (value or key — the
 * key wrapper is the same predicate; the colon rule covers keys).
 * The bytes→facts home: the emitter's style chooser AND the DOM's
 * parse-time fact bit read this one walk, so the verdicts cannot
 * drift. Multibyte-aware (raw NEL/LS/PS reject; a leading U+FEFF
 * rejects; mid-token BOMs ride). */
int yep_scan_plain_safe(const char* p, uint32_t len);

/* Tag-shorthand suffix byte: libyaml's URI set (";/?:@&=+$,_.!~*'()%"
 * plus alphanumerics; flow indicators and ',' terminate instead).
 * Stray non-set bytes stay visible as the props follower, which
 * e_props rejects ("did not find expected whitespace or line break
 * while scanning a tag"). */
int yep_scan_tag_char(unsigned char c);

/* Scans a plain scalar starting at pos (must be content, not a comment).
 * flow != 0 adds flow stop characters; in flow, ':' terminates when
 * followed by blank/EOL/flow indicator. Leading whitespace is NOT
 * consumed. Trailing spaces are trimmed from the span. */
yep_span yep_scan_plain(const char* p, size_t len, size_t pos, int flow);

/* ns-plain-first truth: whether c may START a plain scalar. ','
 * ']' '}' are c-flow-indicators, excluded in every context ('[' '{'
 * never reach a plain start: the engine dispatches flow first). */
int yep_plain_first_ok(unsigned char c);

/* The stop classes (scan.c's SSOT; prebuilt nibble-class tables —
 * '\n','\r',':','#' and, in flow, ',','[',']','{','}').
 * Externally linked so the unit suite can pin them against
 * yep_stopset_init — a hand-written literal drift here would silently
 * end every plain scalar early. */
extern const yep_stopset yep_break_stopset;
extern const yep_stopset yep_plain_stop_block;
extern const yep_stopset yep_plain_stop_flow;

/* The fused line-facts sweep (kernel-dispatched): end/indent/stop in
 * one pass — scan_line and scan_shape both consume these. */
void yep_scan_facts(const char* p, size_t len, size_t pos, yep_line_facts* out);

/* Scans a quoted scalar whose opening quote is at pos. Returns span of
 * the CONTENT (between quotes) and sets *term (EOL on unterminated → the
 * caller errors). q is '\'' or '"'. */
yep_span yep_scan_quoted(const char* p, size_t len, size_t pos, int* has_escape);

/* True if a simple key may start at p[pos] (ns-plain-first approximation
 * plus quote/flow openers). Exposed for the engine's dispatch. */
int yep_scan_is_key_start(unsigned char c);

/* Consumed length of the line break at pos (0 if none): \n -> 1,
 * \r\n -> 2, lone \r -> 1. */
size_t yep_scan_break_len(const char* p, size_t len, size_t pos);
/* NEL/LS/PS length at pos (2/3), 0 otherwise — the multibyte half of
 * yep_scan_break_len's law, callable when only the UTF-8 breaks matter */
size_t yep_scan_mb_break_len(const char* p, size_t len, size_t pos);

/* Line/col bookkeeping across a scanned gap: advances *line and
 * *line_start over the bytes in [*from, to) (only the unscanned region
 * is walked — scanning from the line start is quadratic on long
 * lines). *from becomes `to`. Shared by the flow kernel's event pass
 * and the DOM direct builder so their line/col facts cannot drift. */
void yep_scan_advance_line(const char* p, size_t* from, size_t to, uint32_t* line,
                           size_t* line_start);

#ifdef __cplusplus
}
#endif

#endif /* YEP_SCAN_H */

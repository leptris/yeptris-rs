/* regexp.c — the bounded ECMA-regex subset behind JSON Schema's
 * `pattern` keyword (#882 slice 2; #885 item 2).
 *
 * Supported: literals, '.', [...] classes (ranges, negation, \d\w\s
 * and their negations, \n\t\r\f\v\0, \xHH), groups (capturing ignored,
 * (?:...) fine), alternation, the quantifiers * + ? {n} {n,} {n,m}
 * (lazy markers parse and are ignored — match/no-match has no
 * preference), the ^ $ \b \B assertions. Unsupported constructs
 * (backreferences, lookaround) are SyntaxErrors, and unknown LETTER
 * escapes are identity escapes (non-unicode ECMA-262 Annex B: \p
 * without the u flag is the literal 'p'): the caller treats an invalid
 * pattern as an absent keyword (2020-12's ignore posture), never a
 * guess.
 *
 * Shape: byte-oriented — classes are ASCII, UTF-8 literals match
 * byte-wise (ASCII patterns, the JSON Schema norm, are exact).
 * Parse -> AST -> Thompson NFA -> Pike-style simulation: O(subject *
 * program), NO backtracking blowup, no recursion over the subject.
 * Every array is a fixed stack budget; overflow is a SyntaxError (-1),
 * never a crash. Truth lives here once (MECE: a second consumer moves
 * this to its own module).
 *
 * Two consumption shapes over one compiler (#885 item 3):
 * `yep_re_search` compiles into a stack ctx and drops it (the one-shot
 * walk, zero allocation); the retained-program cache
 * (`yep_re_cache_*`) compiles a schema's patterns ONCE into exact-size
 * heap programs and runs them per validated string — the XSD
 * content-model economy. A cache miss recompiles (identical verdicts);
 * the cache itself is immutable once populated.
 */

#include <string.h>

#include "internal.h"

#define RE_MAX_PAT 1024
#define RE_MAX_AST 256
#define RE_MAX_INST 320
#define RE_MAX_CLS 24
#define RE_AST_DEPTH 64
#define RE_REP_INF 0xFFFFu

enum {
    A_BYTE = 1, /* a: the literal byte */
    A_CLASS,    /* cls */
    A_ASSERT,   /* a: assert kind */
    A_CAT,      /* a, b */
    A_ALT,      /* a, b */
    A_REP,      /* a; min..max */
    A_EMPTY,
};

enum { RE_AS_BOL = 0, RE_AS_EOL, RE_AS_WBOUND, RE_AS_NWBOUND };

typedef struct {
    uint8_t kind;
    uint8_t a, b; /* children / literal byte / assert kind */
    uint8_t cls;  /* class id (A_CLASS) */
    uint8_t lazy; /* parsed, ignored */
    uint16_t min, max;
} re_ast;

typedef struct {
    uint8_t op;  /* RE_OP_* */
    uint8_t cls; /* class id (CHAR) / assert kind (ASSERT) */
    uint16_t x;  /* jump target / literal byte */
    uint16_t y;  /* second jump target (SPLIT) */
} re_inst;

enum { RE_OP_CHAR = 1, RE_OP_BYTE, RE_OP_SPLIT, RE_OP_JMP, RE_OP_ASSERT, RE_OP_MATCH };

/* Pending exit arrows of one compiled fragment: a bitset over
 * (pc * 2 + slot), exactly RE_MAX_INST * 2 bits. frag_connect resolves
 * and CLEARS — an arrow points at exactly one target, once. */
#define RE_FRAG_BYTES ((RE_MAX_INST * 2 + 7) / 8)
typedef struct {
    int in; /* entry pc; -1 = empty (arrows only) */
    uint8_t bits[RE_FRAG_BYTES];
} re_frag;

typedef struct {
    /* parse */
    const char* pat;
    uint32_t pn, pi;
    int err;
    re_ast ast[RE_MAX_AST];
    int ast_n;
    uint8_t cls[RE_MAX_CLS][32];
    int cls_n;
    /* compile */
    re_inst inst[RE_MAX_INST];
    int inst_n;
} re_ctx;

static int re_word(unsigned char b) {
    return (b >= '0' && b <= '9') || (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || b == '_';
}

/* ---- parse ------------------------------------------------------------ */

static int ast_new(re_ctx* c, uint8_t kind) {
    if (c->ast_n >= RE_MAX_AST) {
        c->err = 1;
        return -1;
    }
    int id = c->ast_n++;
    memset(&c->ast[id], 0, sizeof(c->ast[id]));
    c->ast[id].kind = kind;
    c->ast[id].min = 1;
    c->ast[id].max = 1;
    return id;
}

static int cls_new(re_ctx* c) {
    if (c->cls_n >= RE_MAX_CLS) {
        c->err = 1;
        return -1;
    }
    int id = c->cls_n++;
    memset(c->cls[id], 0, 32);
    return id;
}

static void cls_set(re_ctx* c, int id, unsigned b) {
    c->cls[id][(b >> 3) & 31] |= (uint8_t)(1u << (b & 7));
}

static void cls_add_named(re_ctx* c, int id, int neg, int kind) {
    for (int b = 0; b < 256; b++) {
        int m;
        if (kind == 0) {
            m = (b >= '0' && b <= '9');
        } else if (kind == 1) {
            m = (b >= '0' && b <= '9') || (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
                b == '_';
        } else {
            m = b == ' ' || b == '\t' || b == '\n' || b == '\r' || b == '\f' || b == '\v';
        }
        if (neg) {
            m = !m;
        }
        if (m) {
            cls_set(c, id, (unsigned)b);
        }
    }
}

static int p_hex(re_ctx* c, int nibbles) {
    int v = 0;
    for (int i = 0; i < nibbles; i++) {
        if (c->pi >= c->pn) {
            return -1;
        }
        char ch = c->pat[c->pi];
        int d;
        if (ch >= '0' && ch <= '9') {
            d = ch - '0';
        } else if (ch >= 'a' && ch <= 'f') {
            d = ch - 'a' + 10;
        } else if (ch >= 'A' && ch <= 'F') {
            d = ch - 'A' + 10;
        } else {
            return -1;
        }
        v = v * 16 + d;
        c->pi++;
    }
    return v;
}

/* One escape OUTSIDE a class. 0: byte in *out; 1: class id in *out;
 * -2: assertion kind in *out; -1: SyntaxError. */
static int p_escape(re_ctx* c, int* out) {
    if (c->pi >= c->pn) {
        c->err = 1;
        return -1;
    }
    char e = c->pat[c->pi++];
    switch (e) {
    case 'd':
    case 'w':
    case 's': {
        int id = cls_new(c);
        if (id < 0) {
            return -1;
        }
        cls_add_named(c, id, 0, e == 'd' ? 0 : (e == 'w' ? 1 : 2));
        *out = id;
        return 1;
    }
    case 'D':
    case 'W':
    case 'S': {
        int id = cls_new(c);
        if (id < 0) {
            return -1;
        }
        cls_add_named(c, id, 1, e == 'D' ? 0 : (e == 'W' ? 1 : 2));
        *out = id;
        return 1;
    }
    case 'b':
        *out = RE_AS_WBOUND;
        return -2;
    case 'B':
        *out = RE_AS_NWBOUND;
        return -2;
    case 'n':
        *out = '\n';
        return 0;
    case 't':
        *out = '\t';
        return 0;
    case 'r':
        *out = '\r';
        return 0;
    case 'f':
        *out = '\f';
        return 0;
    case 'v':
        *out = '\v';
        return 0;
    case '0':
        *out = 0;
        return 0;
    case 'x': {
        int v = p_hex(c, 2);
        if (v < 0) {
            c->err = 1;
            return -1;
        }
        *out = v;
        return 0;
    }
    default:
        /* non-unicode ECMA-262 (Annex B): unknown LETTER escapes are
         * identity escapes — \p outside unicode mode is the literal
         * 'p' (property escapes need the u flag this kernel does not
         * carry). Digits stay SyntaxError: 1-9 are backreferences,
         * out of the subset — an ignored pattern beats a misread one.
         * Punctuation is identity too. */
        if (e >= '0' && e <= '9') {
            c->err = 1;
            return -1;
        }
        *out = (unsigned char)e;
        return 0;
    }
}

static int p_byte_atom(re_ctx* c, unsigned b) {
    int id = ast_new(c, A_BYTE);
    if (id < 0) {
        return -1;
    }
    c->ast[id].a = (uint8_t)b;
    return id;
}

static int p_class_atom(re_ctx* c, int cid) {
    int id = ast_new(c, A_CLASS);
    if (id < 0) {
        return -1;
    }
    c->ast[id].cls = (uint8_t)cid;
    return id;
}

/* Parses the body of a [...] (the '[' already consumed); -1 on error. */
static int p_class(re_ctx* c) {
    int id = cls_new(c);
    if (id < 0) {
        return -1;
    }
    int neg = 0;
    if (c->pi < c->pn && c->pat[c->pi] == '^') {
        neg = 1;
        c->pi++;
    }
    int first = 1;
    for (;;) {
        if (c->pi >= c->pn) {
            c->err = 1; /* unterminated class */
            return -1;
        }
        char ch = c->pat[c->pi];
        if (ch == ']' && !first) {
            c->pi++;
            break;
        }
        first = 0;
        int lo;
        if (ch == '\\') {
            c->pi++;
            int v = 0;
            int rc = p_escape(c, &v);
            if (rc < 0) {
                c->err = 1; /* \b-as-BACKSPACE unsupported on purpose */
                return -1;
            }
            if (rc == 1) {
                for (int b = 0; b < 256; b++) {
                    if (c->cls[v][(b >> 3) & 31] & (1u << (b & 7))) {
                        cls_set(c, id, (unsigned)b);
                    }
                }
                continue;
            }
            lo = v;
        } else {
            lo = (unsigned char)ch;
            c->pi++;
        }
        if (c->pi + 1 < c->pn && c->pat[c->pi] == '-' && c->pat[c->pi + 1] != ']') {
            c->pi++; /* '-' */
            char hc = c->pat[c->pi];
            int hi;
            if (hc == '\\') {
                c->pi++;
                int v = 0;
                if (p_escape(c, &v) != 0) {
                    c->err = 1;
                    return -1;
                }
                hi = v;
            } else {
                hi = (unsigned char)hc;
                c->pi++;
            }
            if (hi < lo) {
                c->err = 1;
                return -1;
            }
            for (int b = lo; b <= hi; b++) {
                cls_set(c, id, (unsigned)b);
            }
        } else {
            cls_set(c, id, (unsigned)lo);
        }
    }
    if (neg) {
        for (int b = 0; b < 32; b++) {
            c->cls[id][b] = (uint8_t)~c->cls[id][b];
        }
    }
    return id;
}

/* quantifier: 0 none, 1 '*' 2 '+' 3 '?' 4 '{n,m}'; -1 SyntaxError */
static int p_quant(re_ctx* c, uint16_t* min, uint16_t* max, int* lazy) {
    if (c->pi >= c->pn) {
        return 0;
    }
    char q = c->pat[c->pi];
    int kind = 0;
    if (q == '*') {
        kind = 1;
        *min = 0;
        *max = RE_REP_INF;
        c->pi++;
    } else if (q == '+') {
        kind = 2;
        *min = 1;
        *max = RE_REP_INF;
        c->pi++;
    } else if (q == '?') {
        kind = 3;
        *min = 0;
        *max = 1;
        c->pi++;
    } else if (q == '{') {
        uint32_t save = c->pi;
        c->pi++;
        uint32_t n = 0;
        int dn = 0;
        while (c->pi < c->pn && c->pat[c->pi] >= '0' && c->pat[c->pi] <= '9') {
            n = n * 10 + (uint32_t)(c->pat[c->pi] - '0');
            if (n > 255) {
                c->pi = save;
                return 0; /* literal '{' */
            }
            c->pi++;
            dn++;
        }
        if (dn == 0) {
            c->pi = save;
            return 0;
        }
        uint32_t m = n;
        int have_comma = 0;
        if (c->pi < c->pn && c->pat[c->pi] == ',') {
            have_comma = 1;
            c->pi++;
            m = 0;
            int dm = 0;
            while (c->pi < c->pn && c->pat[c->pi] >= '0' && c->pat[c->pi] <= '9') {
                m = m * 10 + (uint32_t)(c->pat[c->pi] - '0');
                if (m > 255) {
                    c->pi = save;
                    return 0;
                }
                c->pi++;
                dm++;
            }
            if (dm == 0) {
                m = RE_REP_INF; /* {n,} */
            }
        }
        if (c->pi >= c->pn || c->pat[c->pi] != '}') {
            c->pi = save;
            return 0; /* literal '{' */
        }
        c->pi++;
        if (!have_comma) {
            m = n; /* {n} */
        }
        if (m != RE_REP_INF && m < n) {
            c->err = 1;
            return -1;
        }
        *min = (uint16_t)n;
        *max = (uint16_t)m;
        kind = 4;
    }
    if (kind != 0) {
        *lazy = 0;
        if (c->pi < c->pn && c->pat[c->pi] == '?') {
            *lazy = 1;
            c->pi++;
        }
    }
    return kind;
}

static int p_alt(re_ctx* c, int depth);

static int p_atom(re_ctx* c, int depth) {
    if (c->pi >= c->pn) {
        c->err = 1;
        return -1;
    }
    char ch = c->pat[c->pi];
    if (ch == '(') {
        c->pi++;
        if (c->pi + 1 < c->pn && c->pat[c->pi] == '?' && c->pat[c->pi + 1] == ':') {
            c->pi += 2;
        } else if (c->pi < c->pn && c->pat[c->pi] == '?') {
            c->err = 1; /* lookaround/flags unsupported */
            return -1;
        }
        int id = p_alt(c, depth + 1);
        if (id < 0) {
            return -1;
        }
        if (c->pi >= c->pn || c->pat[c->pi] != ')') {
            c->err = 1;
            return -1;
        }
        c->pi++;
        return id;
    }
    if (ch == '[') {
        c->pi++;
        int cid = p_class(c);
        return cid < 0 ? -1 : p_class_atom(c, cid);
    }
    if (ch == '.') {
        c->pi++;
        int cid = cls_new(c);
        if (cid < 0) {
            return -1;
        }
        for (int b = 0; b < 256; b++) {
            if (b != '\n') {
                cls_set(c, cid, (unsigned)b);
            }
        }
        return p_class_atom(c, cid);
    }
    if (ch == '^' || ch == '$') {
        c->pi++;
        int id = ast_new(c, A_ASSERT);
        if (id < 0) {
            return -1;
        }
        c->ast[id].a = (ch == '^') ? RE_AS_BOL : RE_AS_EOL;
        return id;
    }
    if (ch == '\\') {
        c->pi++;
        int v = 0;
        int rc = p_escape(c, &v);
        if (rc < 0) {
            if (rc == -2) {
                int id = ast_new(c, A_ASSERT);
                if (id < 0) {
                    return -1;
                }
                c->ast[id].a = (uint8_t)v;
                return id;
            }
            return -1;
        }
        return rc == 1 ? p_class_atom(c, v) : p_byte_atom(c, (unsigned)v);
    }
    if (ch == '*' || ch == '+' || ch == '?') {
        c->err = 1; /* dangling quantifier */
        return -1;
    }
    if (ch == ')' || ch == '|') {
        c->err = 1; /* misplaced terminator */
        return -1;
    }
    c->pi++; /* literal byte (UTF-8 rides byte-wise) */
    return p_byte_atom(c, (unsigned)(unsigned char)ch);
}

static int p_seq(re_ctx* c, int depth) {
    int acc = -1;
    for (;;) {
        if (c->pi >= c->pn) {
            break;
        }
        char ch = c->pat[c->pi];
        if (ch == '|' || ch == ')') {
            break;
        }
        int atom = p_atom(c, depth);
        if (atom < 0) {
            return -1;
        }
        uint16_t qmin = 1, qmax = 1;
        int lazy = 0;
        int q = p_quant(c, &qmin, &qmax, &lazy);
        if (q < 0) {
            return -1;
        }
        if (q > 0) {
            int rep = ast_new(c, A_REP);
            if (rep < 0) {
                return -1;
            }
            c->ast[rep].a = (uint8_t)atom;
            c->ast[rep].min = qmin;
            c->ast[rep].max = qmax;
            c->ast[rep].lazy = (uint8_t)lazy;
            atom = rep;
        }
        if (acc < 0) {
            acc = atom;
        } else {
            int cat = ast_new(c, A_CAT);
            if (cat < 0) {
                return -1;
            }
            c->ast[cat].a = (uint8_t)acc;
            c->ast[cat].b = (uint8_t)atom;
            acc = cat;
        }
    }
    if (acc < 0) {
        acc = ast_new(c, A_EMPTY);
        if (acc < 0) {
            return -1;
        }
    }
    return acc;
}

static int p_alt(re_ctx* c, int depth) {
    if (depth > RE_AST_DEPTH) {
        c->err = 1;
        return -1;
    }
    int first = p_seq(c, depth);
    if (first < 0) {
        return -1;
    }
    int acc = first;
    while (c->pi < c->pn && c->pat[c->pi] == '|') {
        c->pi++;
        int right = p_seq(c, depth);
        if (right < 0) {
            return -1;
        }
        int id = ast_new(c, A_ALT);
        if (id < 0) {
            return -1;
        }
        c->ast[id].a = (uint8_t)acc;
        c->ast[id].b = (uint8_t)right;
        acc = id;
    }
    return acc;
}

/* ---- compile (AST -> NFA) --------------------------------------------- */

static int emit_inst(re_ctx* c, uint8_t op, uint8_t cls, uint16_t x, uint16_t y) {
    if (c->inst_n >= RE_MAX_INST) {
        c->err = 1;
        return -1;
    }
    int pc = c->inst_n++;
    c->inst[pc].op = op;
    c->inst[pc].cls = cls;
    c->inst[pc].x = x;
    c->inst[pc].y = y;
    return pc;
}

static void frag_init(re_frag* f, int in) {
    memset(f->bits, 0, sizeof(f->bits));
    f->in = in;
}

static void frag_bit(re_frag* f, int pc, int slot) {
    int bit = pc * 2 + slot;
    f->bits[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

static void frag_or(re_frag* dst, const re_frag* src) {
    for (int i = 0; i < (int)RE_FRAG_BYTES; i++) {
        dst->bits[i] |= src->bits[i];
    }
}

/* Resolves (and clears) every pending arrow to `target`. */
static void frag_connect(re_frag* f, re_inst* inst, int target) {
    for (int bit = 0; bit < RE_MAX_INST * 2; bit++) {
        if (f->bits[bit >> 3] & (1u << (bit & 7))) {
            int pc = bit >> 1;
            if (bit & 1) {
                inst[pc].y = (uint16_t)target;
            } else {
                inst[pc].x = (uint16_t)target;
            }
        }
    }
    memset(f->bits, 0, sizeof(f->bits));
}

/* Adopts src's pending arrows (dst's own are already resolved). */
static void frag_adopt(re_frag* dst, const re_frag* src) {
    memcpy(dst->bits, src->bits, sizeof(dst->bits));
}

/* An empty fragment still needs an entry instruction to hang its
 * pending exit on: a bare JMP. */
static int frag_force_entry(re_ctx* c, re_frag* f) {
    if (f->in >= 0) {
        return 0;
    }
    int j = emit_inst(c, RE_OP_JMP, 0, 0xFFFF, 0);
    if (j < 0) {
        return -1;
    }
    f->in = j;
    frag_bit(f, j, 0);
    return 0;
}

static int c_node(re_ctx* c, int id, re_frag* out, int depth) {
    if (depth > RE_AST_DEPTH + 8) { /* the left-folded CAT chain runs
                                     * deeper than the parse tree */
        c->err = 1;
        return -1;
    }
    const re_ast* a = &c->ast[id];
    switch (a->kind) {
    case A_EMPTY: {
        frag_init(out, -1);
        return 0;
    }
    case A_BYTE: {
        /* the literal rides cls: x/y are the jump-target pair, and the
         * pending-exit machinery writes x — it must never carry data */
        int pc = emit_inst(c, RE_OP_BYTE, a->a, 0, 0);
        if (pc < 0) {
            return -1;
        }
        frag_init(out, pc);
        frag_bit(out, pc, 0); /* the fall-through exit */
        return 0;
    }
    case A_CLASS: {
        int pc = emit_inst(c, RE_OP_CHAR, a->cls, 0, 0);
        if (pc < 0) {
            return -1;
        }
        frag_init(out, pc);
        frag_bit(out, pc, 0);
        return 0;
    }
    case A_ASSERT: {
        int pc = emit_inst(c, RE_OP_ASSERT, a->a, 0, 0);
        if (pc < 0) {
            return -1;
        }
        frag_init(out, pc);
        frag_bit(out, pc, 0);
        return 0;
    }
    case A_CAT: {
        re_frag f1, f2;
        if (c_node(c, a->a, &f1, depth + 1) < 0 || c_node(c, a->b, &f2, depth + 1) < 0) {
            return -1;
        }
        if (f1.in < 0) {
            *out = f2;
            return 0;
        }
        if (f2.in >= 0) {
            frag_connect(&f1, c->inst, f2.in); /* clears f1's arrows */
            frag_or(&f1, &f2);                 /* f2's pending ride on */
        }
        *out = f1;
        return 0;
    }
    case A_ALT: {
        int split = emit_inst(c, RE_OP_SPLIT, 0, 0xFFFF, 0xFFFF);
        if (split < 0) {
            return -1;
        }
        re_frag f1, f2;
        if (c_node(c, a->a, &f1, depth + 1) < 0 || c_node(c, a->b, &f2, depth + 1) < 0) {
            return -1;
        }
        if (frag_force_entry(c, &f1) < 0 || frag_force_entry(c, &f2) < 0) {
            return -1;
        }
        c->inst[split].x = (uint16_t)f1.in;
        c->inst[split].y = (uint16_t)f2.in;
        frag_init(out, split);
        frag_or(out, &f1);
        frag_or(out, &f2);
        return 0;
    }
    case A_REP: {
        /* the required prefix: min plain copies, left-folded */
        re_frag acc;
        frag_init(&acc, -1);
        for (uint16_t i = 0; i < a->min; i++) {
            re_frag f;
            if (c_node(c, a->a, &f, depth + 1) < 0) {
                return -1;
            }
            if (f.in < 0) {
                continue; /* an empty child requires nothing */
            }
            if (acc.in < 0) {
                acc = f;
            } else {
                frag_connect(&acc, c->inst, f.in); /* prior exits enter
                                                    * this copy */
                frag_adopt(&acc, &f);
            }
        }
        if (a->max == RE_REP_INF) {
            /* star over the child: SPLIT(body, EXIT); body -> SPLIT */
            int split = emit_inst(c, RE_OP_SPLIT, 0, 0xFFFF, 0xFFFF);
            if (split < 0) {
                return -1;
            }
            re_frag f;
            if (c_node(c, a->a, &f, depth + 1) < 0) {
                return -1;
            }
            if (f.in >= 0) {
                c->inst[split].x = (uint16_t)f.in;
                frag_connect(&f, c->inst, split); /* the body loops back */
            } else {
                c->inst[split].x = (uint16_t)split; /* empty body: exit only */
            }
            if (acc.in >= 0) {
                frag_connect(&acc, c->inst, split); /* the prefix enters */
            }
            *out = acc; /* every arrow resolved: bits are clear */
            if (out->in < 0) {
                out->in = split;
            }
            frag_bit(out, split, 1); /* the star's one exit */
            return 0;
        }
        /* the bounded tail: (max - min) optional copies, each
         * SPLIT(body_i, EXIT) — body exits chain into the next copy's
         * split, every EXIT rides pending toward the final target */
        re_frag chain, exits;
        chain = acc; /* the prefix's exits fall into the first split */
        frag_init(&exits, -1);
        uint16_t opt = (uint16_t)(a->max - a->min);
        for (uint16_t i = 0; i < opt; i++) {
            int split = emit_inst(c, RE_OP_SPLIT, 0, 0xFFFF, 0xFFFF);
            if (split < 0) {
                return -1;
            }
            frag_connect(&chain, c->inst, split); /* entries -> this copy */
            re_frag f;
            if (c_node(c, a->a, &f, depth + 1) < 0) {
                return -1;
            }
            if (f.in >= 0) {
                c->inst[split].x = (uint16_t)f.in;
            } else {
                frag_bit(&chain, split, 0); /* empty copy: both arms exit */
            }
            frag_or(&chain, &f);        /* the copy's exits join the chain */
            frag_bit(&exits, split, 1); /* the skip arm */
            if (acc.in < 0) {
                acc.in = split; /* entry = the first split */
            }
        }
        out->in = acc.in;
        frag_init(out, out->in);
        frag_or(out, &chain); /* the last body's exits reach the end */
        frag_or(out, &exits);
        return 0;
    }
    default:
        c->err = 1;
        return -1;
    }
}

/* ---- compile: pattern -> program --------------------------------------- */

/* parse + compile into the caller's ctx: MATCH emits last, every
 * pending arrow resolves. 0 ok (the fragment entry pc in *entry_out),
 * -1 SyntaxError (or a budget trip — the caller treats both as
 * "pattern outside the subset", never a guess). */
static int re_build(re_ctx* c, const char* pat, uint32_t pat_len, int* entry_out) {
    memset(c, 0, sizeof(*c));
    c->pat = pat;
    c->pn = pat_len;
    int root = p_alt(c, 0);
    if (root < 0 || c->pi != c->pn) {
        return -1; /* trailing garbage is a SyntaxError */
    }
    re_frag f;
    if (c_node(c, root, &f, 0) < 0) {
        return -1;
    }
    if (emit_inst(c, RE_OP_MATCH, 0, 0, 0) < 0) {
        return -1;
    }
    frag_connect(&f, c->inst, c->inst_n - 1); /* MATCH emits last */
    if (c->err) {
        return -1;
    }
    *entry_out = f.in;
    return 0;
}

/* ---- the Pike VM (shared by the one-shot and the retained programs) ----- */

/* Runs one compiled program over the subject: 1 match, 0 no match.
 * `entry` is the fragment entry pc (MATCH is emitted last, so the graph
 * is self-contained). Never fails: a program exists only when its
 * pattern compiled. */
static int re_vm(const re_inst* inst, const uint8_t (*cls)[32], int entry, const char* s,
                 uint32_t s_len) {
    uint8_t mark[RE_MAX_INST];
    uint16_t cur[RE_MAX_INST], nxt[RE_MAX_INST];
    /* closure depth: the seeds (m+1: live threads + the unanchored
     * restart) plus one net push per newly marked instruction (m) —
     * 2m+2, not 2m (a 320-instruction program once had one 2-byte
     * overflow slot) */
    uint16_t stack[RE_MAX_INST * 2 + 2];
    uint32_t gen = 1;
    memset(mark, 0, sizeof(mark));

    if (entry < 0) {
        return 1; /* an empty pattern matches everywhere */
    }
    /* initial closure at subject offset 0 (from the fragment entry;
     * MATCH is emitted LAST, so pc 0 is a pattern instruction) */
    int cur_n = 0;
    {
        int st_n = 0;
        stack[st_n++] = (uint16_t)entry;
        while (st_n > 0) {
            uint16_t pc = stack[--st_n];
            if (mark[pc] == (uint8_t)gen) {
                continue;
            }
            mark[pc] = (uint8_t)gen;
            uint8_t op = inst[pc].op;
            if (op == RE_OP_JMP) {
                stack[st_n++] = inst[pc].x;
            } else if (op == RE_OP_SPLIT) {
                stack[st_n++] = inst[pc].x;
                stack[st_n++] = inst[pc].y;
            } else if (op == RE_OP_MATCH) {
                return 1; /* the empty match at offset 0 */
            } else if (op == RE_OP_ASSERT) {
                /* offset 0: BOL always holds; EOL only on the empty
                 * subject; a word boundary holds exactly when the
                 * subject starts inside a word (no previous char) */
                int ok = 0;
                switch (inst[pc].cls) {
                case RE_AS_BOL:
                    ok = 1;
                    break;
                case RE_AS_EOL:
                    ok = (int)s_len == 0;
                    break;
                default: {
                    int wn = s_len > 0 && re_word((unsigned char)s[0]);
                    ok = inst[pc].cls == RE_AS_WBOUND ? (int)wn : !(int)wn;
                    break;
                }
                }
                if (ok) {
                    stack[st_n++] = inst[pc].x; /* the connected exit */
                }
            } else {
                cur[cur_n++] = pc; /* CHAR/BYTE: a live thread */
            }
        }
    }
    for (uint32_t sp = 0; sp < s_len; sp++) {
        gen++;
        if ((gen & 0xFF) == 0) { /* the mark generation wrapped */
            memset(mark, 0, sizeof(mark));
            gen++;
        }
        uint8_t byte = (unsigned char)s[sp];
        int nxt_n = 0;
        for (int i = 0; i < cur_n; i++) {
            uint16_t pc = cur[i];
            int hit = inst[pc].op == RE_OP_BYTE
                          ? (inst[pc].cls == byte)
                          : (cls[inst[pc].cls][(byte >> 3) & 31] & (1u << (byte & 7))) != 0;
            if (hit) {
                /* the exit is the CONNECTED target (frag_connect), not
                 * a fall-through: fragments link arbitrary pcs */
                nxt[nxt_n++] = inst[pc].x;
            }
        }
        nxt[nxt_n++] = (uint16_t)entry; /* unanchored: the search restarts
                                         * at every offset */
        gen++;
        if ((gen & 0xFF) == 0) {
            memset(mark, 0, sizeof(mark));
            gen++;
        }
        /* closure at subject offset sp + 1 */
        int st_n = 0, live_n = 0, matched = 0;
        for (int i = 0; i < nxt_n; i++) {
            stack[st_n++] = nxt[i];
        }
        while (st_n > 0) {
            uint16_t pc = stack[--st_n];
            if (mark[pc] == (uint8_t)gen) {
                continue;
            }
            mark[pc] = (uint8_t)gen;
            uint8_t op = inst[pc].op;
            if (op == RE_OP_JMP) {
                stack[st_n++] = inst[pc].x;
            } else if (op == RE_OP_SPLIT) {
                stack[st_n++] = inst[pc].x;
                stack[st_n++] = inst[pc].y;
            } else if (op == RE_OP_MATCH) {
                matched = 1;
            } else if (op == RE_OP_ASSERT) {
                int ok = 0;
                switch (inst[pc].cls) {
                case RE_AS_BOL:
                    ok = (sp + 1) == 0;
                    break;
                case RE_AS_EOL:
                    ok = (int)(sp + 1) == (int)s_len;
                    break;
                default: {
                    int wp = re_word((unsigned char)s[sp]);
                    int wn = (sp + 1) < (uint32_t)s_len && re_word((unsigned char)s[sp + 1]);
                    ok = inst[pc].cls == RE_AS_WBOUND ? (wp != wn) : (wp == wn);
                    break;
                }
                }
                if (ok) {
                    stack[st_n++] = inst[pc].x; /* the connected exit */
                }
            } else {
                nxt[live_n++] = pc; /* a live thread for the next byte */
            }
        }
        if (matched) {
            return 1;
        }
        memcpy(cur, nxt, (size_t)live_n * sizeof(uint16_t));
        cur_n = live_n;
    }
    return 0;
}

/* ---- the one-shot search (compile fresh, run, drop) --------------------- */

int yep_re_search(const char* pat, uint32_t pat_len, const char* s, uint32_t s_len) {
    if (pat == NULL || pat_len == 0 || pat_len > RE_MAX_PAT) {
        return -1;
    }
    re_ctx c;
    int entry = -1;
    if (re_build(&c, pat, pat_len, &entry) != 0) {
        return -1;
    }
    return re_vm(c.inst, c.cls, entry, s, s_len);
}

/* ---- the retained programs (the compiled schema's pattern cache) -------- */

/* A compiled program: exact-size copies of the instruction stream and
 * the class table (the ctx's bounded budgets, retained past the
 * compile call). */
typedef struct {
    re_inst* inst;
    uint8_t (*cls)[32];
    int inst_n, cls_n;
    int entry;
} re_prog;

static void re_prog_free(re_prog* p) {
    const yep_allocator* sys = yep_system_allocator();
    yep_free(sys, p->inst);
    yep_free(sys, p->cls);
    p->inst = NULL;
    p->cls = NULL;
}

/* Compiles into a heap program. 0 ok; -1 SyntaxError or allocation
 * failure (nothing retained; both read as "the pattern is unusable",
 * the kernel's ignore posture). */
static int re_compile(const char* pat, uint32_t pat_len, re_prog* out) {
    if (pat == NULL || pat_len == 0 || pat_len > RE_MAX_PAT) {
        return -1;
    }
    re_ctx c;
    int entry = -1;
    if (re_build(&c, pat, pat_len, &entry) != 0) {
        return -1;
    }
    const yep_allocator* sys = yep_system_allocator();
    re_prog p;
    memset(&p, 0, sizeof(p));
    p.inst_n = c.inst_n;
    p.cls_n = c.cls_n;
    p.entry = entry;
    p.inst = yep_alloc(sys, (size_t)c.inst_n * sizeof(re_inst));
    p.cls = yep_alloc(sys, (size_t)(c.cls_n > 0 ? c.cls_n : 1) * 32);
    if (p.inst == NULL || p.cls == NULL) {
        re_prog_free(&p);
        return -1;
    }
    memcpy(p.inst, c.inst, (size_t)c.inst_n * sizeof(re_inst));
    memcpy(p.cls, c.cls, (size_t)c.cls_n * 32);
    *out = p;
    return 0;
}

/* The table: pattern source -> compiled program. Keys are the schema's
 * own byte views (stable for the handle's lifetime); lookups compare
 * content so equal patterns share one program. Bounded: over
 * RE_CACHE_MAX entries a miss recompiles — the one-shot behavior. */
#define RE_CACHE_MAX 256
typedef struct {
    const char* pat;
    uint32_t plen;
    re_prog prog;
} re_cache_slot;

struct yep_re_cache {
    re_cache_slot slot[RE_CACHE_MAX];
    int n;
};

yep_re_cache* yep_re_cache_new(void) {
    yep_re_cache* t = yep_alloc(yep_system_allocator(), sizeof(yep_re_cache));
    if (t != NULL) {
        memset(t, 0, sizeof(*t)); /* the allocator does not zero: n starts empty */
    }
    return t;
}

void yep_re_cache_free(yep_re_cache* t) {
    if (t == NULL) {
        return;
    }
    for (int i = 0; i < t->n; i++) {
        re_prog_free(&t->slot[i].prog);
    }
    yep_free(yep_system_allocator(), t);
}

int yep_re_cache_add(yep_re_cache* t, const char* pat, uint32_t plen) {
    if (t == NULL || pat == NULL || plen == 0) {
        return 0; /* nothing to retain */
    }
    for (int i = 0; i < t->n; i++) {
        if (t->slot[i].plen == plen && memcmp(t->slot[i].pat, pat, plen) == 0) {
            return 1; /* already retained (equal patterns share one program) */
        }
    }
    if (t->n >= RE_CACHE_MAX) {
        return 0; /* the table is full: misses recompile per use, as the
                   * one-shot face always has */
    }
    re_prog p;
    if (re_compile(pat, plen, &p) != 0) {
        return 0; /* invalid pattern: not retained; every use reads -1,
                   * exactly the uncached verdict */
    }
    t->slot[t->n].pat = pat;
    t->slot[t->n].plen = plen;
    t->slot[t->n].prog = p;
    t->n++;
    return 1;
}

int yep_re_cache_search(const yep_re_cache* t, const char* pat, uint32_t plen, const char* s,
                        uint32_t s_len) {
    if (t != NULL && pat != NULL) {
        for (int i = 0; i < t->n; i++) {
            if (t->slot[i].plen == plen && memcmp(t->slot[i].pat, pat, plen) == 0) {
                return re_vm(t->slot[i].prog.inst, t->slot[i].prog.cls, t->slot[i].prog.entry, s,
                             s_len);
            }
        }
    }
    /* a miss (or no cache: the one-shot face) compiles fresh — identical
     * verdicts either way, and the handle stays untouched */
    return yep_re_search(pat, plen, s, s_len);
}

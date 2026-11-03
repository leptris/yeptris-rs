/* scalars.c — finishing implementations. Escape truth declared here once. */

#include <string.h>

#include "scalars.h"

static char* yep_emit_cp(char* o, uint32_t cp) {
    if (cp < 0x80) {
        *o++ = (char)cp;
    } else if (cp < 0x800) {
        *o++ = (char)(0xC0 | (cp >> 6));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        *o++ = (char)(0xE0 | (cp >> 12));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *o++ = (char)(0xF0 | (cp >> 18));
        *o++ = (char)(0x80 | ((cp >> 12) & 0x3F));
        *o++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *o++ = (char)(0x80 | (cp & 0x3F));
    }
    return o;
}

/* Grows into the pool via a scratch buffer, returns final pool block. */
typedef struct yep_buf {
    yep_pool* pool;
    char* data;
    size_t len, cap;
} yep_buf;

static int yep_buf_reserve(yep_buf* b, size_t need) {
    if (b->len + need <= b->cap) {
        return 1;
    }
    if (b->pool == NULL) {
        return 0; /* fixed buffer: in-place decoding never exceeds cap */
    }
    size_t ncap = b->cap ? b->cap * 2 : 256;
    while (ncap < b->len + need) {
        ncap *= 2;
    }
    char* nd = yep_pool_alloc(b->pool, ncap, 16);
    if (nd == NULL) {
        return 0;
    }
    if (b->len > 0) {
        memcpy(nd, b->data, b->len);
    }
    b->data = nd;
    b->cap = ncap;
    return 1;
}

static void yep_buf_putc(yep_buf* b, char c) {
    if (yep_buf_reserve(b, 1)) {
        b->data[b->len++] = c;
    }
}

static void yep_buf_put(yep_buf* b, const char* s, size_t n) {
    if (n && yep_buf_reserve(b, n)) {
        memcpy(b->data + b->len, s, n);
        b->len += n;
    }
}

static uint32_t finish_double_body(const char* p, uint32_t start, uint32_t end, int multiline,
                                   yep_buf* b);

char* yep_finish_double(const char* p, uint32_t start, uint32_t end, int multiline, yep_pool* pool,
                        uint32_t* out_len) {
    yep_buf b = {pool, NULL, 0, 0};
    *out_len = finish_double_body(p, start, end, multiline, &b);
    return b.data;
}

/* Decodes into a caller buffer (no pool): decoding only shrinks, so
 * cap = the raw span is a hard bound. Returns the decoded length. */
uint32_t yep_finish_double_into(const char* p, uint32_t start, uint32_t end, char* dst,
                                uint32_t cap) {
    yep_buf b = {NULL, dst, 0, cap};
    return finish_double_body(p, start, end, 0, &b);
}

static uint32_t finish_double_body(const char* p, uint32_t start, uint32_t end, int multiline,
                                   yep_buf* b) {
    size_t esc_floor = 0; /* decoded bytes below this are escape content */
    size_t i = start;
    while (i < end) {
        unsigned char c = (unsigned char)p[i];
        if (c != '\\') {
            if (multiline && (c == '\n' || c == '\r')) {
                /* fold: one break → ' ', n → (n-1) '\n'. Trailing
                 * whitespace BEFORE a break is stripped; the next line's
                 * leading whitespace (spaces AND tabs) is skipped; a
                 * whitespace-ONLY line counts as an empty line. */
                uint32_t breaks = 0;
                for (;;) {
                    while (i < end && (p[i] == '\n' || p[i] == '\r')) {
                        if (p[i] == '\r' && i + 1 < end && p[i + 1] == '\n') {
                            i++;
                        }
                        i++;
                        breaks++;
                    }
                    size_t save = i;
                    while (i < end && (p[i] == ' ' || p[i] == '\t')) {
                        i++;
                    }
                    if (i < end && (p[i] == '\n' || p[i] == '\r') && i > save) {
                        continue; /* whitespace-only line: keep counting */
                    }
                    break;
                }
                while (b->len > esc_floor &&
                       (b->data[b->len - 1] == ' ' || b->data[b->len - 1] == '\t')) {
                    b->len--; /* raw trailing white space strips; escapes keep */
                }
                if (breaks == 1) {
                    yep_buf_putc(b, ' ');
                } else {
                    for (uint32_t k = 1; k < breaks; k++) {
                        yep_buf_putc(b, '\n');
                    }
                }
                continue;
            }
            yep_buf_putc(b, (char)c);
            i++;
            continue;
        }
        /* escape */
        i++;
        if (i >= end) {
            break; /* trailing backslash before closing quote — treated literally */
        }
        unsigned char e = (unsigned char)p[i];
        switch (e) {
        case '0':
            yep_buf_putc(b, '\0');
            i++;
            break;
        case 'a':
            yep_buf_putc(b, '\a');
            i++;
            break;
        case 'b':
            yep_buf_putc(b, '\b');
            i++;
            break;
        case 't':
        case '\t':
            yep_buf_putc(b, '\t');
            i++;
            break;
        case 'n':
            yep_buf_putc(b, '\n');
            i++;
            break;
        case 'v':
            yep_buf_putc(b, '\v');
            i++;
            break;
        case 'f':
            yep_buf_putc(b, '\f');
            i++;
            break;
        case 'r':
            yep_buf_putc(b, '\r');
            i++;
            break;
        case 'e':
            yep_buf_putc(b, 0x1B);
            i++;
            break;
        case ' ':
            yep_buf_putc(b, ' ');
            i++;
            break;
        case '"':
            yep_buf_putc(b, '"');
            i++;
            break;
        case '/':
            yep_buf_putc(b, '/');
            i++;
            break;
        case '\\':
            yep_buf_putc(b, '\\');
            i++;
            break;
        case 'N': /* U+0085 NEL */ {
            char tmp[2] = {(char)(unsigned char)0xC2, (char)(unsigned char)0x85};
            yep_buf_put(b, tmp, 2);
            i++;
            break;
        }
        case '_': /* U+00A0 */ {
            char tmp[2] = {(char)(unsigned char)0xC2, (char)(unsigned char)0xA0};
            yep_buf_put(b, tmp, 2);
            i++;
            break;
        }
        case 'L': /* U+2028 */ {
            char tmp[3] = {(char)(unsigned char)0xE2, (char)(unsigned char)0x80,
                           (char)(unsigned char)0xA8};
            yep_buf_put(b, tmp, 3);
            i++;
            break;
        }
        case 'P': /* U+2029 */ {
            char tmp[3] = {(char)(unsigned char)0xE2, (char)(unsigned char)0x80,
                           (char)(unsigned char)0xA9};
            yep_buf_put(b, tmp, 3);
            i++;
            break;
        }
        case 'x':
        case 'u':
        case 'U': {
            int digits = (e == 'x') ? 2 : (e == 'u') ? 4 : 8;
            uint32_t cp = 0;
            int ok = 1;
            for (int k = 0; k < digits; k++) {
                int hv =
                    (i + 1 + (size_t)k < end) ? hexval((unsigned char)p[i + 1 + (size_t)k]) : -1;
                if (hv < 0) {
                    ok = 0;
                    break;
                }
                cp = (cp << 4) | (uint32_t)hv;
            }
            if (!ok) {
                yep_buf_putc(b, '\\');
                yep_buf_putc(b, (char)e);
                i++;
                break;
            }
            if (e == 'u' && cp >= 0xD800 && cp <= 0xDBFF && i + 1 + 4 + 6 <= end &&
                p[i + 1 + 4] == '\\' && p[i + 2 + 4] == 'u') {
                /* surrogate pair: combine into one scalar */
                uint32_t lo = 0;
                int lok = 1;
                for (int k = 0; k < 4; k++) {
                    int hv2 = hexval((unsigned char)p[i + 3 + 4 + (size_t)k]);
                    if (hv2 < 0) {
                        lok = 0;
                        break;
                    }
                    lo = (lo << 4) | (uint32_t)hv2;
                }
                if (lok && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6; /* the low escape's "\u" */
                }
            }
            if (cp >= 0xD800 && cp <= 0xDFFF) {
                /* lone surrogate: validators reject; decode defensively */
                cp = 0xFFFD;
            }
            char tmp[4];
            char* o = yep_emit_cp(tmp, cp);
            yep_buf_put(b, tmp, (size_t)(o - tmp));
            i += 1 + (size_t)digits;
            break;
        }
        case '\n':
        case '\r': {
            /* Escaped line break: the break itself is removed, but every
             * break after it (whitespace-only lines included) contributes
             * one newline, and the next line's leading whitespace is
             * stripped. Folding the remaining run with the plain N-1 rule
             * loses one break (libyaml and libfyaml agree on this count). */
            size_t j = i;
            if (p[j] == '\r' && j + 1 < end && p[j + 1] == '\n') {
                j++;
            }
            j++;
            uint32_t extra = 0;
            for (;;) {
                while (j < end && (p[j] == ' ' || p[j] == '\t')) {
                    j++;
                }
                if (j < end && (p[j] == '\n' || p[j] == '\r')) {
                    if (p[j] == '\r' && j + 1 < end && p[j + 1] == '\n') {
                        j++;
                    }
                    j++;
                    extra++;
                    continue;
                }
                break;
            }
            for (uint32_t k = 0; k < extra; k++) {
                yep_buf_putc(b, '\n');
            }
            i = j;
            break;
        }
        default:
            /* unknown escape: keep verbatim */
            yep_buf_putc(b, '\\');
            yep_buf_putc(b, (char)e);
            i++;
            break;
        }
        esc_floor = b->len; /* decoded content is never strippable */
    }
    return (uint32_t)b->len;
}

char* yep_finish_single(const char* p, uint32_t start, uint32_t end, int multiline, yep_pool* pool,
                        uint32_t* out_len) {
    /* Count '' occurrences and breaks to decide necessity. */
    int changed = 0;
    for (size_t i = start; i + 1 < end; i++) {
        if (p[i] == '\'' && p[i + 1] == '\'') {
            changed = 1;
            break;
        }
    }
    if (!changed && !multiline) {
        return NULL; /* borrow: content already final */
    }

    yep_buf b = {pool, NULL, 0, 0};
    size_t i = start;
    while (i < end) {
        if (p[i] == '\'' && i + 1 < end && p[i + 1] == '\'') {
            yep_buf_putc(&b, '\'');
            i += 2;
            continue;
        }
        if (multiline && (p[i] == '\n' || p[i] == '\r')) {
            uint32_t breaks = 0;
            for (;;) {
                while (i < end && (p[i] == '\n' || p[i] == '\r')) {
                    if (p[i] == '\r' && i + 1 < end && p[i + 1] == '\n') {
                        i++;
                    }
                    i++;
                    breaks++;
                }
                size_t save = i;
                while (i < end && (p[i] == ' ' || p[i] == '\t')) {
                    i++;
                }
                if (i < end && (p[i] == '\n' || p[i] == '\r') && i > save) {
                    continue; /* whitespace-only line: keep counting */
                }
                break;
            }
            while (b.len > 0 && (b.data[b.len - 1] == ' ' || b.data[b.len - 1] == '\t')) {
                b.len--; /* trailing white space strips before breaks */
            }
            if (breaks == 1) {
                yep_buf_putc(&b, ' ');
            } else {
                for (uint32_t k = 1; k < breaks; k++) {
                    yep_buf_putc(&b, '\n');
                }
            }
            continue;
        }
        yep_buf_putc(&b, p[i]);
        i++;
    }
    *out_len = (uint32_t)b.len;
    return b.data;
}

static void yep_emit_breaks(yep_buf* b, uint32_t n) {
    for (uint32_t k = 0; k < n; k++) {
        yep_buf_putc(b, '\n');
    }
}

/* Plain folding: 0 breaks → nothing, 1 break → ' ', n>1 → (n-1) '\n'. */
static void yep_fold_break(yep_buf* b, uint32_t breaks) {
    if (breaks == 0) {
        return;
    }
    if (breaks == 1) {
        yep_buf_putc(b, ' ');
    } else {
        yep_emit_breaks(b, breaks - 1);
    }
}

char* yep_fold_plain(const yep_fold_line* lines, size_t n, yep_pool* pool, uint32_t* out_len) {
    yep_buf b = {pool, NULL, 0, 0};
    for (size_t i = 0; i < n; i++) {
        yep_fold_break(&b, lines[i].breaks_before);
        yep_buf_put(&b, lines[i].content.p, lines[i].content.len);
    }
    *out_len = (uint32_t)b.len;
    return b.data;
}

char* yep_finish_block(const yep_fold_line* lines, size_t n, int folded, int chomp,
                       uint32_t trailing_breaks, yep_pool* pool, uint32_t* out_len) {
    yep_buf b = {pool, NULL, 0, 0};
    for (size_t i = 0; i < n; i++) {
        uint32_t br = lines[i].breaks_before;
        if (!folded) {
            yep_emit_breaks(&b, br); /* literal: every break is a newline */
        } else {
            /* folded: plain-fold, unless a more-indented neighbor forces
             * every break to be kept literally. LEADING breaks (before
             * the first content line) have no preceding content to fold
             * against — they emit literally. */
            int keep = lines[i].more_indented || (i > 0 && lines[i - 1].more_indented);
            if (keep || i == 0) {
                yep_emit_breaks(&b, br);
            } else {
                yep_fold_break(&b, br);
            }
        }
        yep_buf_put(&b, lines[i].content.p, lines[i].content.len);
    }

    /* Chomping. */
    if (chomp == 1) {        /* strip: nothing */
    } else if (chomp == 2) { /* keep: every trailing break */
        yep_emit_breaks(&b, trailing_breaks);
    } else { /* clip: one trailing newline, but nothing without content */
        if (b.len > 0) {
            yep_buf_putc(&b, '\n');
        }
    }
    *out_len = (uint32_t)b.len;
    return b.data;
}

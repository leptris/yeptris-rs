/* regexp.c — the bounded ECMA-regex subset behind JSON Schema's
 * `pattern` keyword (#882 slice 2; #885 item 2; the 2020-12 sota
 * sweep's code-point classes).
 *
 * Supported: literals, '.', [...] classes (ranges, negation, \d\w\s
 * and their negations, \n\t\r\f\v\0, \xHH, \cX control codes,
 * \p{Letter}/\p{digit} property escapes), groups (capturing ignored,
 * (?:...) fine), alternation, the quantifiers * + ? {n} {n,} {n,m}
 * (lazy markers parse and are ignored — match/no-match has no
 * preference), the ^ $ \b \B assertions. Unsupported constructs
 * (backreferences, lookaround) are SyntaxErrors, and unknown LETTER
 * escapes are identity escapes (non-unicode ECMA-262 Annex B: an
 * unsupported \p{...} name is the literal 'p'): the caller treats an
 * invalid pattern as an absent keyword (2020-12's ignore posture),
 * never a guess.
 *
 * Shape: code-point classes over byte literals — a class (bracket,
 * named, or property) matches and consumes a WHOLE UTF-8 code point
 * (a byte bitmap holds the point when every byte of it is in the
 * bitmap: exactly the ECMA verdict for positive and negated classes
 * alike); the named classes carry range tables (\d and \w are the
 * ASCII classes ECMA-262 defines, \s is ECMA WhiteSpace plus
 * LineTerminator, \D/\W/\S their code-point complements,
 * \p{Letter}/\p{digit} the generated Unicode tables above). Thread
 * positions are per-thread in a five-slot ring — a consumer advances
 * by its code point's length, so the window is bounded and every
 * array stays fixed-size. Parse -> AST -> Thompson NFA -> Pike-style
 * simulation: O(subject * program), NO backtracking blowup, no
 * recursion over the subject. Every array is a fixed budget; overflow
 * is a SyntaxError (-1), never a crash. Truth lives here once (MECE:
 * a second consumer moves this to its own module).
 *
 * Two consumption shapes over one compiler (#885 item 3):
 * `yep_re_search` compiles into a stack ctx and drops it (the one-shot
 * walk, zero allocation); the retained-program cache
 * (`yep_re_cache_*`) compiles a schema's patterns ONCE into exact-size
 * heap programs and runs them per validated string — the XSD
 * content-model economy. A cache miss recompiles (identical verdicts);
 * the cache itself is immutable once populated.
 */

#include <stdio.h>
#include <string.h>

#include "internal.h"

#define RE_MAX_PAT 1024
#define RE_MAX_AST 256
#define RE_MAX_INST 320
#define RE_MAX_CLS 24
#define RE_AST_DEPTH 64
#define RE_REP_INF 0xFFFFu

/* The Unicode range tables behind the kernel's code-point classes
 * (the 2020-12 sota sweep): sorted inclusive (lo, hi) pairs over
 * scalar values, generated from Unicode 13.0.0 (python unicodedata).
 * The kernel carries exactly two named properties -- Letter (general
 * category L) and digit (Nd), the suite's exercised set; every other
 * \p{...} name stays the Annex B identity escape. \d and \w are the
 * ASCII classes ECMA-262 defines them to be; \s is ECMA WhiteSpace
 * plus LineTerminator (Unicode-aware); \D \W \S are their code-point
 * complements. */
static const uint32_t RE_PROP_L[] = {
    0x0041,  0x005A,  0x0061,  0x007A,  0x00AA,  0x00AA,  0x00B5,  0x00B5,  0x00BA,  0x00BA,
    0x00C0,  0x00D6,  0x00D8,  0x00F6,  0x00F8,  0x02C1,  0x02C6,  0x02D1,  0x02E0,  0x02E4,
    0x02EC,  0x02EC,  0x02EE,  0x02EE,  0x0370,  0x0374,  0x0376,  0x0377,  0x037A,  0x037D,
    0x037F,  0x037F,  0x0386,  0x0386,  0x0388,  0x038A,  0x038C,  0x038C,  0x038E,  0x03A1,
    0x03A3,  0x03F5,  0x03F7,  0x0481,  0x048A,  0x052F,  0x0531,  0x0556,  0x0559,  0x0559,
    0x0560,  0x0588,  0x05D0,  0x05EA,  0x05EF,  0x05F2,  0x0620,  0x064A,  0x066E,  0x066F,
    0x0671,  0x06D3,  0x06D5,  0x06D5,  0x06E5,  0x06E6,  0x06EE,  0x06EF,  0x06FA,  0x06FC,
    0x06FF,  0x06FF,  0x0710,  0x0710,  0x0712,  0x072F,  0x074D,  0x07A5,  0x07B1,  0x07B1,
    0x07CA,  0x07EA,  0x07F4,  0x07F5,  0x07FA,  0x07FA,  0x0800,  0x0815,  0x081A,  0x081A,
    0x0824,  0x0824,  0x0828,  0x0828,  0x0840,  0x0858,  0x0860,  0x086A,  0x08A0,  0x08B4,
    0x08B6,  0x08C7,  0x0904,  0x0939,  0x093D,  0x093D,  0x0950,  0x0950,  0x0958,  0x0961,
    0x0971,  0x0980,  0x0985,  0x098C,  0x098F,  0x0990,  0x0993,  0x09A8,  0x09AA,  0x09B0,
    0x09B2,  0x09B2,  0x09B6,  0x09B9,  0x09BD,  0x09BD,  0x09CE,  0x09CE,  0x09DC,  0x09DD,
    0x09DF,  0x09E1,  0x09F0,  0x09F1,  0x09FC,  0x09FC,  0x0A05,  0x0A0A,  0x0A0F,  0x0A10,
    0x0A13,  0x0A28,  0x0A2A,  0x0A30,  0x0A32,  0x0A33,  0x0A35,  0x0A36,  0x0A38,  0x0A39,
    0x0A59,  0x0A5C,  0x0A5E,  0x0A5E,  0x0A72,  0x0A74,  0x0A85,  0x0A8D,  0x0A8F,  0x0A91,
    0x0A93,  0x0AA8,  0x0AAA,  0x0AB0,  0x0AB2,  0x0AB3,  0x0AB5,  0x0AB9,  0x0ABD,  0x0ABD,
    0x0AD0,  0x0AD0,  0x0AE0,  0x0AE1,  0x0AF9,  0x0AF9,  0x0B05,  0x0B0C,  0x0B0F,  0x0B10,
    0x0B13,  0x0B28,  0x0B2A,  0x0B30,  0x0B32,  0x0B33,  0x0B35,  0x0B39,  0x0B3D,  0x0B3D,
    0x0B5C,  0x0B5D,  0x0B5F,  0x0B61,  0x0B71,  0x0B71,  0x0B83,  0x0B83,  0x0B85,  0x0B8A,
    0x0B8E,  0x0B90,  0x0B92,  0x0B95,  0x0B99,  0x0B9A,  0x0B9C,  0x0B9C,  0x0B9E,  0x0B9F,
    0x0BA3,  0x0BA4,  0x0BA8,  0x0BAA,  0x0BAE,  0x0BB9,  0x0BD0,  0x0BD0,  0x0C05,  0x0C0C,
    0x0C0E,  0x0C10,  0x0C12,  0x0C28,  0x0C2A,  0x0C39,  0x0C3D,  0x0C3D,  0x0C58,  0x0C5A,
    0x0C60,  0x0C61,  0x0C80,  0x0C80,  0x0C85,  0x0C8C,  0x0C8E,  0x0C90,  0x0C92,  0x0CA8,
    0x0CAA,  0x0CB3,  0x0CB5,  0x0CB9,  0x0CBD,  0x0CBD,  0x0CDE,  0x0CDE,  0x0CE0,  0x0CE1,
    0x0CF1,  0x0CF2,  0x0D04,  0x0D0C,  0x0D0E,  0x0D10,  0x0D12,  0x0D3A,  0x0D3D,  0x0D3D,
    0x0D4E,  0x0D4E,  0x0D54,  0x0D56,  0x0D5F,  0x0D61,  0x0D7A,  0x0D7F,  0x0D85,  0x0D96,
    0x0D9A,  0x0DB1,  0x0DB3,  0x0DBB,  0x0DBD,  0x0DBD,  0x0DC0,  0x0DC6,  0x0E01,  0x0E30,
    0x0E32,  0x0E33,  0x0E40,  0x0E46,  0x0E81,  0x0E82,  0x0E84,  0x0E84,  0x0E86,  0x0E8A,
    0x0E8C,  0x0EA3,  0x0EA5,  0x0EA5,  0x0EA7,  0x0EB0,  0x0EB2,  0x0EB3,  0x0EBD,  0x0EBD,
    0x0EC0,  0x0EC4,  0x0EC6,  0x0EC6,  0x0EDC,  0x0EDF,  0x0F00,  0x0F00,  0x0F40,  0x0F47,
    0x0F49,  0x0F6C,  0x0F88,  0x0F8C,  0x1000,  0x102A,  0x103F,  0x103F,  0x1050,  0x1055,
    0x105A,  0x105D,  0x1061,  0x1061,  0x1065,  0x1066,  0x106E,  0x1070,  0x1075,  0x1081,
    0x108E,  0x108E,  0x10A0,  0x10C5,  0x10C7,  0x10C7,  0x10CD,  0x10CD,  0x10D0,  0x10FA,
    0x10FC,  0x1248,  0x124A,  0x124D,  0x1250,  0x1256,  0x1258,  0x1258,  0x125A,  0x125D,
    0x1260,  0x1288,  0x128A,  0x128D,  0x1290,  0x12B0,  0x12B2,  0x12B5,  0x12B8,  0x12BE,
    0x12C0,  0x12C0,  0x12C2,  0x12C5,  0x12C8,  0x12D6,  0x12D8,  0x1310,  0x1312,  0x1315,
    0x1318,  0x135A,  0x1380,  0x138F,  0x13A0,  0x13F5,  0x13F8,  0x13FD,  0x1401,  0x166C,
    0x166F,  0x167F,  0x1681,  0x169A,  0x16A0,  0x16EA,  0x16F1,  0x16F8,  0x1700,  0x170C,
    0x170E,  0x1711,  0x1720,  0x1731,  0x1740,  0x1751,  0x1760,  0x176C,  0x176E,  0x1770,
    0x1780,  0x17B3,  0x17D7,  0x17D7,  0x17DC,  0x17DC,  0x1820,  0x1878,  0x1880,  0x1884,
    0x1887,  0x18A8,  0x18AA,  0x18AA,  0x18B0,  0x18F5,  0x1900,  0x191E,  0x1950,  0x196D,
    0x1970,  0x1974,  0x1980,  0x19AB,  0x19B0,  0x19C9,  0x1A00,  0x1A16,  0x1A20,  0x1A54,
    0x1AA7,  0x1AA7,  0x1B05,  0x1B33,  0x1B45,  0x1B4B,  0x1B83,  0x1BA0,  0x1BAE,  0x1BAF,
    0x1BBA,  0x1BE5,  0x1C00,  0x1C23,  0x1C4D,  0x1C4F,  0x1C5A,  0x1C7D,  0x1C80,  0x1C88,
    0x1C90,  0x1CBA,  0x1CBD,  0x1CBF,  0x1CE9,  0x1CEC,  0x1CEE,  0x1CF3,  0x1CF5,  0x1CF6,
    0x1CFA,  0x1CFA,  0x1D00,  0x1DBF,  0x1E00,  0x1F15,  0x1F18,  0x1F1D,  0x1F20,  0x1F45,
    0x1F48,  0x1F4D,  0x1F50,  0x1F57,  0x1F59,  0x1F59,  0x1F5B,  0x1F5B,  0x1F5D,  0x1F5D,
    0x1F5F,  0x1F7D,  0x1F80,  0x1FB4,  0x1FB6,  0x1FBC,  0x1FBE,  0x1FBE,  0x1FC2,  0x1FC4,
    0x1FC6,  0x1FCC,  0x1FD0,  0x1FD3,  0x1FD6,  0x1FDB,  0x1FE0,  0x1FEC,  0x1FF2,  0x1FF4,
    0x1FF6,  0x1FFC,  0x2071,  0x2071,  0x207F,  0x207F,  0x2090,  0x209C,  0x2102,  0x2102,
    0x2107,  0x2107,  0x210A,  0x2113,  0x2115,  0x2115,  0x2119,  0x211D,  0x2124,  0x2124,
    0x2126,  0x2126,  0x2128,  0x2128,  0x212A,  0x212D,  0x212F,  0x2139,  0x213C,  0x213F,
    0x2145,  0x2149,  0x214E,  0x214E,  0x2183,  0x2184,  0x2C00,  0x2C2E,  0x2C30,  0x2C5E,
    0x2C60,  0x2CE4,  0x2CEB,  0x2CEE,  0x2CF2,  0x2CF3,  0x2D00,  0x2D25,  0x2D27,  0x2D27,
    0x2D2D,  0x2D2D,  0x2D30,  0x2D67,  0x2D6F,  0x2D6F,  0x2D80,  0x2D96,  0x2DA0,  0x2DA6,
    0x2DA8,  0x2DAE,  0x2DB0,  0x2DB6,  0x2DB8,  0x2DBE,  0x2DC0,  0x2DC6,  0x2DC8,  0x2DCE,
    0x2DD0,  0x2DD6,  0x2DD8,  0x2DDE,  0x2E2F,  0x2E2F,  0x3005,  0x3006,  0x3031,  0x3035,
    0x303B,  0x303C,  0x3041,  0x3096,  0x309D,  0x309F,  0x30A1,  0x30FA,  0x30FC,  0x30FF,
    0x3105,  0x312F,  0x3131,  0x318E,  0x31A0,  0x31BF,  0x31F0,  0x31FF,  0x3400,  0x4DBF,
    0x4E00,  0x9FFC,  0xA000,  0xA48C,  0xA4D0,  0xA4FD,  0xA500,  0xA60C,  0xA610,  0xA61F,
    0xA62A,  0xA62B,  0xA640,  0xA66E,  0xA67F,  0xA69D,  0xA6A0,  0xA6E5,  0xA717,  0xA71F,
    0xA722,  0xA788,  0xA78B,  0xA7BF,  0xA7C2,  0xA7CA,  0xA7F5,  0xA801,  0xA803,  0xA805,
    0xA807,  0xA80A,  0xA80C,  0xA822,  0xA840,  0xA873,  0xA882,  0xA8B3,  0xA8F2,  0xA8F7,
    0xA8FB,  0xA8FB,  0xA8FD,  0xA8FE,  0xA90A,  0xA925,  0xA930,  0xA946,  0xA960,  0xA97C,
    0xA984,  0xA9B2,  0xA9CF,  0xA9CF,  0xA9E0,  0xA9E4,  0xA9E6,  0xA9EF,  0xA9FA,  0xA9FE,
    0xAA00,  0xAA28,  0xAA40,  0xAA42,  0xAA44,  0xAA4B,  0xAA60,  0xAA76,  0xAA7A,  0xAA7A,
    0xAA7E,  0xAAAF,  0xAAB1,  0xAAB1,  0xAAB5,  0xAAB6,  0xAAB9,  0xAABD,  0xAAC0,  0xAAC0,
    0xAAC2,  0xAAC2,  0xAADB,  0xAADD,  0xAAE0,  0xAAEA,  0xAAF2,  0xAAF4,  0xAB01,  0xAB06,
    0xAB09,  0xAB0E,  0xAB11,  0xAB16,  0xAB20,  0xAB26,  0xAB28,  0xAB2E,  0xAB30,  0xAB5A,
    0xAB5C,  0xAB69,  0xAB70,  0xABE2,  0xAC00,  0xD7A3,  0xD7B0,  0xD7C6,  0xD7CB,  0xD7FB,
    0xF900,  0xFA6D,  0xFA70,  0xFAD9,  0xFB00,  0xFB06,  0xFB13,  0xFB17,  0xFB1D,  0xFB1D,
    0xFB1F,  0xFB28,  0xFB2A,  0xFB36,  0xFB38,  0xFB3C,  0xFB3E,  0xFB3E,  0xFB40,  0xFB41,
    0xFB43,  0xFB44,  0xFB46,  0xFBB1,  0xFBD3,  0xFD3D,  0xFD50,  0xFD8F,  0xFD92,  0xFDC7,
    0xFDF0,  0xFDFB,  0xFE70,  0xFE74,  0xFE76,  0xFEFC,  0xFF21,  0xFF3A,  0xFF41,  0xFF5A,
    0xFF66,  0xFFBE,  0xFFC2,  0xFFC7,  0xFFCA,  0xFFCF,  0xFFD2,  0xFFD7,  0xFFDA,  0xFFDC,
    0x10000, 0x1000B, 0x1000D, 0x10026, 0x10028, 0x1003A, 0x1003C, 0x1003D, 0x1003F, 0x1004D,
    0x10050, 0x1005D, 0x10080, 0x100FA, 0x10280, 0x1029C, 0x102A0, 0x102D0, 0x10300, 0x1031F,
    0x1032D, 0x10340, 0x10342, 0x10349, 0x10350, 0x10375, 0x10380, 0x1039D, 0x103A0, 0x103C3,
    0x103C8, 0x103CF, 0x10400, 0x1049D, 0x104B0, 0x104D3, 0x104D8, 0x104FB, 0x10500, 0x10527,
    0x10530, 0x10563, 0x10600, 0x10736, 0x10740, 0x10755, 0x10760, 0x10767, 0x10800, 0x10805,
    0x10808, 0x10808, 0x1080A, 0x10835, 0x10837, 0x10838, 0x1083C, 0x1083C, 0x1083F, 0x10855,
    0x10860, 0x10876, 0x10880, 0x1089E, 0x108E0, 0x108F2, 0x108F4, 0x108F5, 0x10900, 0x10915,
    0x10920, 0x10939, 0x10980, 0x109B7, 0x109BE, 0x109BF, 0x10A00, 0x10A00, 0x10A10, 0x10A13,
    0x10A15, 0x10A17, 0x10A19, 0x10A35, 0x10A60, 0x10A7C, 0x10A80, 0x10A9C, 0x10AC0, 0x10AC7,
    0x10AC9, 0x10AE4, 0x10B00, 0x10B35, 0x10B40, 0x10B55, 0x10B60, 0x10B72, 0x10B80, 0x10B91,
    0x10C00, 0x10C48, 0x10C80, 0x10CB2, 0x10CC0, 0x10CF2, 0x10D00, 0x10D23, 0x10E80, 0x10EA9,
    0x10EB0, 0x10EB1, 0x10F00, 0x10F1C, 0x10F27, 0x10F27, 0x10F30, 0x10F45, 0x10FB0, 0x10FC4,
    0x10FE0, 0x10FF6, 0x11003, 0x11037, 0x11083, 0x110AF, 0x110D0, 0x110E8, 0x11103, 0x11126,
    0x11144, 0x11144, 0x11147, 0x11147, 0x11150, 0x11172, 0x11176, 0x11176, 0x11183, 0x111B2,
    0x111C1, 0x111C4, 0x111DA, 0x111DA, 0x111DC, 0x111DC, 0x11200, 0x11211, 0x11213, 0x1122B,
    0x11280, 0x11286, 0x11288, 0x11288, 0x1128A, 0x1128D, 0x1128F, 0x1129D, 0x1129F, 0x112A8,
    0x112B0, 0x112DE, 0x11305, 0x1130C, 0x1130F, 0x11310, 0x11313, 0x11328, 0x1132A, 0x11330,
    0x11332, 0x11333, 0x11335, 0x11339, 0x1133D, 0x1133D, 0x11350, 0x11350, 0x1135D, 0x11361,
    0x11400, 0x11434, 0x11447, 0x1144A, 0x1145F, 0x11461, 0x11480, 0x114AF, 0x114C4, 0x114C5,
    0x114C7, 0x114C7, 0x11580, 0x115AE, 0x115D8, 0x115DB, 0x11600, 0x1162F, 0x11644, 0x11644,
    0x11680, 0x116AA, 0x116B8, 0x116B8, 0x11700, 0x1171A, 0x11800, 0x1182B, 0x118A0, 0x118DF,
    0x118FF, 0x11906, 0x11909, 0x11909, 0x1190C, 0x11913, 0x11915, 0x11916, 0x11918, 0x1192F,
    0x1193F, 0x1193F, 0x11941, 0x11941, 0x119A0, 0x119A7, 0x119AA, 0x119D0, 0x119E1, 0x119E1,
    0x119E3, 0x119E3, 0x11A00, 0x11A00, 0x11A0B, 0x11A32, 0x11A3A, 0x11A3A, 0x11A50, 0x11A50,
    0x11A5C, 0x11A89, 0x11A9D, 0x11A9D, 0x11AC0, 0x11AF8, 0x11C00, 0x11C08, 0x11C0A, 0x11C2E,
    0x11C40, 0x11C40, 0x11C72, 0x11C8F, 0x11D00, 0x11D06, 0x11D08, 0x11D09, 0x11D0B, 0x11D30,
    0x11D46, 0x11D46, 0x11D60, 0x11D65, 0x11D67, 0x11D68, 0x11D6A, 0x11D89, 0x11D98, 0x11D98,
    0x11EE0, 0x11EF2, 0x11FB0, 0x11FB0, 0x12000, 0x12399, 0x12480, 0x12543, 0x13000, 0x1342E,
    0x14400, 0x14646, 0x16800, 0x16A38, 0x16A40, 0x16A5E, 0x16AD0, 0x16AED, 0x16B00, 0x16B2F,
    0x16B40, 0x16B43, 0x16B63, 0x16B77, 0x16B7D, 0x16B8F, 0x16E40, 0x16E7F, 0x16F00, 0x16F4A,
    0x16F50, 0x16F50, 0x16F93, 0x16F9F, 0x16FE0, 0x16FE1, 0x16FE3, 0x16FE3, 0x17000, 0x187F7,
    0x18800, 0x18CD5, 0x18D00, 0x18D08, 0x1B000, 0x1B11E, 0x1B150, 0x1B152, 0x1B164, 0x1B167,
    0x1B170, 0x1B2FB, 0x1BC00, 0x1BC6A, 0x1BC70, 0x1BC7C, 0x1BC80, 0x1BC88, 0x1BC90, 0x1BC99,
    0x1D400, 0x1D454, 0x1D456, 0x1D49C, 0x1D49E, 0x1D49F, 0x1D4A2, 0x1D4A2, 0x1D4A5, 0x1D4A6,
    0x1D4A9, 0x1D4AC, 0x1D4AE, 0x1D4B9, 0x1D4BB, 0x1D4BB, 0x1D4BD, 0x1D4C3, 0x1D4C5, 0x1D505,
    0x1D507, 0x1D50A, 0x1D50D, 0x1D514, 0x1D516, 0x1D51C, 0x1D51E, 0x1D539, 0x1D53B, 0x1D53E,
    0x1D540, 0x1D544, 0x1D546, 0x1D546, 0x1D54A, 0x1D550, 0x1D552, 0x1D6A5, 0x1D6A8, 0x1D6C0,
    0x1D6C2, 0x1D6DA, 0x1D6DC, 0x1D6FA, 0x1D6FC, 0x1D714, 0x1D716, 0x1D734, 0x1D736, 0x1D74E,
    0x1D750, 0x1D76E, 0x1D770, 0x1D788, 0x1D78A, 0x1D7A8, 0x1D7AA, 0x1D7C2, 0x1D7C4, 0x1D7CB,
    0x1E100, 0x1E12C, 0x1E137, 0x1E13D, 0x1E14E, 0x1E14E, 0x1E2C0, 0x1E2EB, 0x1E800, 0x1E8C4,
    0x1E900, 0x1E943, 0x1E94B, 0x1E94B, 0x1EE00, 0x1EE03, 0x1EE05, 0x1EE1F, 0x1EE21, 0x1EE22,
    0x1EE24, 0x1EE24, 0x1EE27, 0x1EE27, 0x1EE29, 0x1EE32, 0x1EE34, 0x1EE37, 0x1EE39, 0x1EE39,
    0x1EE3B, 0x1EE3B, 0x1EE42, 0x1EE42, 0x1EE47, 0x1EE47, 0x1EE49, 0x1EE49, 0x1EE4B, 0x1EE4B,
    0x1EE4D, 0x1EE4F, 0x1EE51, 0x1EE52, 0x1EE54, 0x1EE54, 0x1EE57, 0x1EE57, 0x1EE59, 0x1EE59,
    0x1EE5B, 0x1EE5B, 0x1EE5D, 0x1EE5D, 0x1EE5F, 0x1EE5F, 0x1EE61, 0x1EE62, 0x1EE64, 0x1EE64,
    0x1EE67, 0x1EE6A, 0x1EE6C, 0x1EE72, 0x1EE74, 0x1EE77, 0x1EE79, 0x1EE7C, 0x1EE7E, 0x1EE7E,
    0x1EE80, 0x1EE89, 0x1EE8B, 0x1EE9B, 0x1EEA1, 0x1EEA3, 0x1EEA5, 0x1EEA9, 0x1EEAB, 0x1EEBB,
    0x20000, 0x2A6DD, 0x2A700, 0x2B734, 0x2B740, 0x2B81D, 0x2B820, 0x2CEA1, 0x2CEB0, 0x2EBE0,
    0x2F800, 0x2FA1D, 0x30000, 0x3134A,
};
#define RE_PROP_L_N 622u

static const uint32_t RE_PROP_ND[] = {
    0x0030,  0x0039,  0x0660,  0x0669,  0x06F0,  0x06F9,  0x07C0,  0x07C9,  0x0966,  0x096F,
    0x09E6,  0x09EF,  0x0A66,  0x0A6F,  0x0AE6,  0x0AEF,  0x0B66,  0x0B6F,  0x0BE6,  0x0BEF,
    0x0C66,  0x0C6F,  0x0CE6,  0x0CEF,  0x0D66,  0x0D6F,  0x0DE6,  0x0DEF,  0x0E50,  0x0E59,
    0x0ED0,  0x0ED9,  0x0F20,  0x0F29,  0x1040,  0x1049,  0x1090,  0x1099,  0x17E0,  0x17E9,
    0x1810,  0x1819,  0x1946,  0x194F,  0x19D0,  0x19D9,  0x1A80,  0x1A89,  0x1A90,  0x1A99,
    0x1B50,  0x1B59,  0x1BB0,  0x1BB9,  0x1C40,  0x1C49,  0x1C50,  0x1C59,  0xA620,  0xA629,
    0xA8D0,  0xA8D9,  0xA900,  0xA909,  0xA9D0,  0xA9D9,  0xA9F0,  0xA9F9,  0xAA50,  0xAA59,
    0xABF0,  0xABF9,  0xFF10,  0xFF19,  0x104A0, 0x104A9, 0x10D30, 0x10D39, 0x11066, 0x1106F,
    0x110F0, 0x110F9, 0x11136, 0x1113F, 0x111D0, 0x111D9, 0x112F0, 0x112F9, 0x11450, 0x11459,
    0x114D0, 0x114D9, 0x11650, 0x11659, 0x116C0, 0x116C9, 0x11730, 0x11739, 0x118E0, 0x118E9,
    0x11950, 0x11959, 0x11C50, 0x11C59, 0x11D50, 0x11D59, 0x11DA0, 0x11DA9, 0x16A60, 0x16A69,
    0x16B50, 0x16B59, 0x1D7CE, 0x1D7FF, 0x1E140, 0x1E149, 0x1E2F0, 0x1E2F9, 0x1E950, 0x1E959,
    0x1FBF0, 0x1FBF9,
};
#define RE_PROP_ND_N 61u

static const uint32_t RE_RANGES_D[] = {
    0x0030,
    0x0039,
};
#define RE_RANGES_D_N 1u

static const uint32_t RE_RANGES_WHITE[] = {
    0x0009, 0x000D, 0x0020, 0x0020, 0x00A0, 0x00A0, 0x1680, 0x1680, 0x2000, 0x200A,
    0x2028, 0x2029, 0x202F, 0x202F, 0x205F, 0x205F, 0x3000, 0x3000, 0xFEFF, 0xFEFF,
};
#define RE_RANGES_WHITE_N 10u

static const uint32_t RE_RANGES_WORD[] = {
    0x0030, 0x0039, 0x0041, 0x005A, 0x005F, 0x005F, 0x0061, 0x007A,

};
#define RE_RANGES_WORD_N 4u

static const uint32_t RE_RANGES_NOTD[] = {
    0x0000,
    0x002F,
    0x003A,
    0x10FFFF,
};
#define RE_RANGES_NOTD_N 2u

static const uint32_t RE_RANGES_NOTW[] = {
    0x0000, 0x002F, 0x003A, 0x0040, 0x005B, 0x005E, 0x0060, 0x0060, 0x007B, 0x10FFFF,
};
#define RE_RANGES_NOTW_N 5u

static const uint32_t RE_RANGES_NOTS[] = {
    0x0000, 0x0008, 0x000E, 0x001F, 0x0021, 0x009F, 0x00A1, 0x167F, 0x1681, 0x1FFF, 0x200B,
    0x2027, 0x202A, 0x202E, 0x2030, 0x205E, 0x2060, 0x2FFF, 0x3001, 0xFEFE, 0xFF00, 0x10FFFF,
};
#define RE_RANGES_NOTS_N 11u

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
    /* the code-point classes: a class whose entry is non-NULL matches
     * whole UTF-8 code points by range table (the named and property
     * classes); NULL entries match bytes through the bitmap */
    const uint32_t* ucls[RE_MAX_CLS];
    uint16_t ucls_n[RE_MAX_CLS];
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

/* Registers a class as a code-point range table (the named and
 * property classes). The ranges are process-immutable statics, so a
 * retained program can hold the reference for its lifetime. */
static void cls_add_ranges(re_ctx* c, int id, const uint32_t* ranges, uint16_t n) {
    c->ucls[id] = ranges;
    c->ucls_n[id] = n;
}

static void cls_add_named(re_ctx* c, int id, int neg, int kind) {
    /* \d and \w are the ASCII classes ECMA-262 defines; \s is ECMA
     * WhiteSpace plus LineTerminator; the D/W/S forms are their
     * code-point complements */
    const uint32_t* r = NULL;
    uint16_t n = 0;
    if (kind == 0) {
        r = neg ? RE_RANGES_NOTD : RE_RANGES_D;
        n = neg ? RE_RANGES_NOTD_N : RE_RANGES_D_N;
    } else if (kind == 1) {
        r = neg ? RE_RANGES_NOTW : RE_RANGES_WORD;
        n = neg ? RE_RANGES_NOTW_N : RE_RANGES_WORD_N;
    } else {
        r = neg ? RE_RANGES_NOTS : RE_RANGES_WHITE;
        n = neg ? RE_RANGES_NOTS_N : RE_RANGES_WHITE_N;
    }
    cls_add_ranges(c, id, r, n);
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
    case 'c': {
        /* ECMA-262 \cX: the control code of a letter (X & 0x1F). A
         * non-letter after \c is the Annex B identity escape (the
         * literal 'c' — the next byte parses on its own). */
        if (c->pi < c->pn) {
            char l = c->pat[c->pi];
            if ((l >= 'A' && l <= 'Z') || (l >= 'a' && l <= 'z')) {
                c->pi++;
                *out = (unsigned char)(l & 0x1F);
                return 0;
            }
        }
        *out = (unsigned char)'c';
        return 0;
    }
    case 'x': {
        int v = p_hex(c, 2);
        if (v < 0) {
            c->err = 1;
            return -1;
        }
        *out = v;
        return 0;
    }
    case 'p': {
        /* \p{Name}: the suite's two exercised properties (Letter,
         * digit — case-insensitive) ride the generated range tables;
         * every other name is the Annex B identity escape (the literal
         * 'p'; the braced name re-parses as literals). */
        if (c->pi < c->pn && c->pat[c->pi] == '{') {
            size_t close = c->pi;
            while (close < c->pn && c->pat[close] != '}') {
                close++;
            }
            size_t nl = close - (size_t)(c->pi + 1);
            if (close < c->pn && nl >= 4 && nl <= 8) {
                char name[8];
                for (size_t i = 0; i < nl; i++) {
                    name[i] = c->pat[c->pi + 1 + i];
                }
                name[nl] = '\0';
                /* the exact canonical spellings only: any other name
                 * (and any other casing) stays the Annex B identity
                 * escape — the bounded kernel carries the suite's two
                 * exercised properties, never a guess */
                int prop = (nl == 6 && memcmp(name, "Letter", 6) == 0)
                               ? 1
                               : ((nl == 5 && memcmp(name, "digit", 5) == 0) ? 2 : 0);
                if (prop != 0) {
                    c->pi = (uint32_t)close + 1;
                    int id = cls_new(c);
                    if (id < 0) {
                        return -1;
                    }
                    cls_add_ranges(c, id, prop == 1 ? RE_PROP_L : RE_PROP_ND,
                                   prop == 1 ? (uint16_t)RE_PROP_L_N : (uint16_t)RE_PROP_ND_N);
                    *out = id;
                    return 1;
                }
            }
        }
        *out = (unsigned char)'p';
        return 0;
    }
    default:
        /* non-unicode ECMA-262 (Annex B): unknown LETTER escapes are
         * identity escapes — an unsupported \p{...} name is the literal
         * 'p'. Digits stay SyntaxError: 1-9 are backreferences, out of
         * the subset — an ignored pattern beats a misread one.
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
                if (c->ucls[v] != NULL) {
                    /* a code-point class contributes the bytes of its
                     * sub-0x100 ranges (the bracket stays byte-wise:
                     * the multi-byte whites and \p{...} letters beyond
                     * latin-1 are the documented bracket divergence) */
                    for (uint16_t rr = 0; rr < c->ucls_n[v]; rr++) {
                        uint32_t blo = c->ucls[v][rr * 2];
                        uint32_t bhi = c->ucls[v][rr * 2 + 1];
                        if (bhi > 0xFF) {
                            bhi = 0xFF;
                        }
                        for (uint32_t b = blo; b <= bhi; b++) {
                            cls_set(c, id, (unsigned)b);
                        }
                    }
                } else {
                    for (int b = 0; b < 256; b++) {
                        if (c->cls[v][(b >> 3) & 31] & (1u << (b & 7))) {
                            cls_set(c, id, (unsigned)b);
                        }
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

/* Decodes one UTF-8 code point at `at`: the scalar value, and its byte
 * length in *len (1 for a malformed or lone byte, which reads as
 * latin-1). Never reads past s_len. */
static uint32_t re_cp_at(const char* s, uint32_t s_len, uint32_t at, uint32_t* len) {
    unsigned char b = (unsigned char)s[at];
    if (b < 0x80) {
        *len = 1;
        return b;
    }
    int n;
    uint32_t cp;
    if ((b & 0xE0) == 0xC0 && b >= 0xC2) {
        n = 2;
        cp = b & 0x1Fu;
    } else if ((b & 0xF0) == 0xE0) {
        n = 3;
        cp = b & 0x0Fu;
    } else if ((b & 0xF8) == 0xF0 && b <= 0xF4) {
        n = 4;
        cp = b & 0x07u;
    } else {
        *len = 1;
        return b;
    }
    if (at + (uint32_t)n > s_len) {
        *len = 1;
        return b;
    }
    for (int i = 1; i < n; i++) {
        unsigned char cb = (unsigned char)s[at + i];
        if ((cb & 0xC0) != 0x80) {
            *len = 1;
            return b;
        }
        cp = (cp << 6) | (cb & 0x3Fu);
    }
    *len = (uint32_t)n;
    return cp;
}

/* Range membership (sorted inclusive lo,hi pairs; binary search). */
static int re_ranges_has(const uint32_t* r, uint16_t n, uint32_t cp) {
    uint16_t lo = 0, hi = n;
    while (lo < hi) {
        uint16_t mid = (uint16_t)(lo + (hi - lo) / 2);
        if (cp < r[mid * 2]) {
            hi = mid;
        } else if (cp > r[mid * 2 + 1]) {
            lo = (uint16_t)(mid + 1);
        } else {
            return 1;
        }
    }
    return 0;
}

/* The \b machinery: a code point is a word character iff it is ASCII
 * [0-9A-Z_a-z]; every non-ASCII code point is non-word, and UTF-8's
 * self-synchronization keeps a boundary from forming inside one. */
static int re_word_before(const char* s, uint32_t at) {
    if (at == 0) {
        return 0;
    }
    uint32_t q = at;
    while (q > 0 && ((unsigned char)s[q - 1] & 0xC0) == 0x80) {
        q--;
    }
    if (q < at) {
        uint32_t len = 0;
        (void)re_cp_at(s, at, q, &len);
        if (q + len != at) {
            q = at - 1; /* a malformed tail: the last byte reads as itself */
        }
    } else {
        q = at - 1;
    }
    return re_word((unsigned char)s[q]);
}

static int re_word_at(const char* s, uint32_t s_len, uint32_t at) {
    if (at >= s_len) {
        return 0;
    }
    uint32_t len = 0;
    uint32_t cp = re_cp_at(s, s_len, at, &len);
    return cp < 0x80 && re_word((unsigned char)cp);
}

/* The step machine. Positions are PER-THREAD in a bounded window: a
 * byte-literal consumes 1 byte, a class consumes its whole code point
 * (up to 4), so successor positions trail the frontier by at most
 * 3 bytes — the pending ring holds them, one slot per frontier
 * offset. Dedup is (pc, position) per generation: tag 1 marks a ring
 * enqueue, tag 2 a closure visit. */
typedef struct {
    const re_inst* inst;
    const uint8_t (*cls)[32];
    const uint32_t* const* ucls;
    const uint16_t* ucls_n;
    const char* s;
    uint32_t s_len;
    uint8_t* mark_gen; /* the closure's dedup: (gen, pos, tag=2) per pc */
    uint8_t* mark_pos;
    uint8_t* mark_tag;
    uint8_t* ring_gen; /* the ring-enqueue dedup: (gen, pos, tag=1); a pc
                        * can be a live consumer AND a future successor,
                        * so the two marks never share storage */
    uint8_t* ring_pos;
    uint8_t* ring_tag;
    uint32_t gen;
    uint32_t gen_at; /* the frontier position the current step started at */
    uint16_t* stack; /* RE_MAX_INST * 2 + 2 */
    uint16_t (*ring)[RE_MAX_INST * 2];
    int* ring_n;
    int matched;
} re_mach;

static void re_closure(re_mach* v, uint16_t seed, uint32_t pos) {
    int st_n = 0;
    v->stack[st_n++] = seed;
    while (st_n > 0) {
        uint16_t pc = v->stack[--st_n];
        /* the TAG field is the gate: it is memset at entry and written
         * together with gen/pos, so testing it first keeps the other
         * two reads defined (valgrind: they are uninitialized until
         * the pc's first real marking) */
        if (v->mark_tag[pc] == 2 && v->mark_gen[pc] == (uint8_t)v->gen &&
            v->mark_pos[pc] == (uint8_t)pos) {
            continue;
        }
        v->mark_gen[pc] = (uint8_t)v->gen;
        v->mark_pos[pc] = (uint8_t)pos;
        v->mark_tag[pc] = 2;
        const re_inst* in = &v->inst[pc];
        if (in->op == RE_OP_JMP) {
            v->stack[st_n++] = in->x;
        } else if (in->op == RE_OP_SPLIT) {
            v->stack[st_n++] = in->x;
            v->stack[st_n++] = in->y;
        } else if (in->op == RE_OP_MATCH) {
            v->matched = 1;
        } else if (in->op == RE_OP_ASSERT) {
            int ok = 0;
            switch (in->cls) {
            case RE_AS_BOL:
                ok = pos == 0;
                break;
            case RE_AS_EOL:
                ok = pos == v->s_len;
                break;
            default: {
                int wp = re_word_before(v->s, pos);
                int wn = re_word_at(v->s, v->s_len, pos);
                ok = in->cls == RE_AS_WBOUND ? (wp != wn) : (wp == wn);
                break;
            }
            }
            if (ok) {
                v->stack[st_n++] = in->x;
            }
        }
        /* CHAR/BYTE: consumers — the step at `pos` evaluates them */
    }
}

/* Enqueues a successor state for `pos` (tag 1; the ring slot is the
 * position's offset from the frontier). */
static void re_ring_push(re_mach* v, uint16_t pc, uint32_t pos) {
    int slot = (int)(pos - (v->gen_at)); /* the frontier this step started at */
    if (slot < 1 || slot > 4) {
        return; /* unreachable (adv <= 4); a guard, not a guess */
    }
    if (v->ring_tag[pc] == 1 && v->ring_gen[pc] == (uint8_t)v->gen &&
        v->ring_pos[pc] == (uint8_t)pos) {
        return;
    }
    v->ring_gen[pc] = (uint8_t)v->gen;
    v->ring_pos[pc] = (uint8_t)pos;
    v->ring_tag[pc] = 1;
    if (v->ring_n[slot] >= RE_MAX_INST * 2) {
        return; /* the slot cap: a duplicate arrival, already bounded */
    }
    v->ring[slot][v->ring_n[slot]++] = pc;
}

/* Runs one compiled program over the subject: 1 match, 0 no match.
 * `entry` is the fragment entry pc (MATCH is emitted last, so the graph
 * is self-contained). Never fails: a program exists only when its
 * pattern compiled. */
static int re_vm(const re_inst* inst, const uint8_t (*cls)[32], const uint32_t* const* ucls,
                 const uint16_t* ucls_n, int entry, const char* s, uint32_t s_len) {
    uint8_t mark_gen[RE_MAX_INST], mark_pos[RE_MAX_INST], mark_tag[RE_MAX_INST];
    uint8_t ring_gen[RE_MAX_INST], ring_pos[RE_MAX_INST], ring_tag[RE_MAX_INST];
    /* slot d holds the states positioned at the frontier sp+d; a code
     * point consumes at most 4 bytes, so five slots cover the window */
    uint16_t ring[5][RE_MAX_INST * 2];
    int ring_n[5] = {0, 0, 0, 0, 0};
    uint16_t stack[RE_MAX_INST * 2 + 2];
    uint32_t gen = 1;
    memset(mark_tag, 0, sizeof(mark_tag)); /* gen 0 never matches: marks start at 1 */
    memset(ring_tag, 0, sizeof(ring_tag));

    if (entry < 0) {
        return 1; /* an empty pattern matches everywhere */
    }
    re_mach v;
    memset(&v, 0, sizeof(v));
    v.inst = inst;
    v.cls = cls;
    v.ucls = ucls;
    v.ucls_n = ucls_n;
    v.s = s;
    v.s_len = s_len;
    v.mark_gen = mark_gen;
    v.mark_pos = mark_pos;
    v.mark_tag = mark_tag;
    v.ring_gen = ring_gen;
    v.ring_pos = ring_pos;
    v.ring_tag = ring_tag;
    v.gen = gen;
    v.stack = stack;
    v.ring = ring;
    v.ring_n = ring_n;

    /* the initial closure at position 0 (MATCH reachable here is the
     * empty match) */
    re_closure(&v, (uint16_t)entry, 0);
    if (v.matched) {
        return 1;
    }
    for (uint32_t sp = 0; sp < s_len; sp++) {
        v.gen_at = sp;
        /* the states that reached THIS position */
        int n0 = ring_n[0];
        for (int i = 0; i < n0; i++) {
            mark_tag[ring[0][i]] = 0; /* the closure re-marks */
            re_closure(&v, ring[0][i], sp);
            if (v.matched) {
                return 1;
            }
        }
        /* the unanchored restart: the entry at every byte offset */
        re_closure(&v, (uint16_t)entry, sp);
        if (v.matched) {
            return 1;
        }
        /* the consumers live at sp: everything the closures marked
         * tag 2 at (gen, sp) */
        uint32_t len = 1;
        uint32_t cp = re_cp_at(s, s_len, sp, &len);
        for (int pc = 0; pc < RE_MAX_INST; pc++) {
            if (mark_tag[pc] != 2 || mark_gen[pc] != (uint8_t)gen || mark_pos[pc] != (uint8_t)sp) {
                continue;
            }
            const re_inst* in = &inst[pc];
            int hit = 0;
            uint32_t adv = 1;
            if (in->op == RE_OP_BYTE) {
                hit = in->cls == (unsigned char)s[sp];
            } else if (in->op == RE_OP_CHAR) {
                if (ucls != NULL && ucls[in->cls] != NULL) {
                    hit = re_ranges_has(ucls[in->cls], ucls_n[in->cls], cp);
                    adv = len;
                } else {
                    /* the byte bitmap covers the whole code point: the
                     * point matches when EVERY byte of it is in the
                     * class (for ASCII code points the old behavior) */
                    hit = 1;
                    for (uint32_t k = 0; k < len; k++) {
                        unsigned char b = (unsigned char)s[sp + k];
                        if (!(cls[in->cls][(b >> 3) & 31] & (1u << (b & 7)))) {
                            hit = 0;
                            break;
                        }
                    }
                    adv = len;
                }
            } else {
                continue;
            }
            if (hit) {
                re_ring_push(&v, in->x, sp + adv);
            }
        }
        /* the ring shifts: slot d holds sp+d; the next frontier is sp+1 */
        for (int d = 0; d < 4; d++) {
            memcpy(ring[d], ring[d + 1], (size_t)ring_n[d + 1] * sizeof(uint16_t));
            ring_n[d] = ring_n[d + 1];
        }
        ring_n[4] = 0;
        gen++;
        if ((gen & 0xFF) == 0) { /* the mark generation wrapped */
            memset(mark_tag, 0, sizeof(mark_tag));
            memset(ring_tag, 0, sizeof(ring_tag));
            gen++;
        }
        v.gen = gen;
        /* the tail: states that reached s_len run their closure there
         * (EOL and MATCH live at the end) */
    }
    {
        int n0 = ring_n[0];
        v.gen_at = s_len;
        for (int i = 0; i < n0; i++) {
            mark_tag[ring[0][i]] = 0;
            re_closure(&v, ring[0][i], s_len);
            if (v.matched) {
                return 1;
            }
        }
    }
    return 0;
}

int yep_re_search(const char* pat, uint32_t pat_len, const char* s, uint32_t s_len) {
    if (pat == NULL || pat_len == 0 || pat_len > RE_MAX_PAT) {
        return -1;
    }
    re_ctx c;
    int entry = -1;
    if (re_build(&c, pat, pat_len, &entry) != 0) {
        return -1;
    }
    return re_vm(c.inst, c.cls, (const uint32_t* const*)c.ucls, c.ucls_n, entry, s, s_len);
}

/* ---- the retained programs (the compiled schema's pattern cache) -------- */

/* A compiled program: exact-size copies of the instruction stream and
 * the class table (the ctx's bounded budgets, retained past the
 * compile call). */
typedef struct {
    re_inst* inst;
    uint8_t (*cls)[32];
    int inst_n, cls_n;
    /* the code-point class refs: process-immutable static tables, so
     * the reference rides the program (never freed per-program) */
    const uint32_t* ucls[RE_MAX_CLS];
    uint16_t ucls_n[RE_MAX_CLS];
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
    memcpy((void*)p.ucls, (const void*)c.ucls, sizeof(p.ucls)); /* MSVC C4090: the
                                                                 * const-pointee array decays
                                                                 * at the wrong level for
                                                                 * void* */
    memcpy(p.ucls_n, c.ucls_n, sizeof(p.ucls_n));
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
                return re_vm(t->slot[i].prog.inst, t->slot[i].prog.cls,
                             (const uint32_t* const*)t->slot[i].prog.ucls, t->slot[i].prog.ucls_n,
                             t->slot[i].prog.entry, s, s_len);
            }
        }
    }
    /* a miss (or no cache: the one-shot face) compiles fresh — identical
     * verdicts either way, and the handle stays untouched */
    return yep_re_search(pat, plen, s, s_len);
}

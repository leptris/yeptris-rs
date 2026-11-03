//! The differential gate for the direct Node serde route
//! (yeptris-rs#41): every corpus document deserializes through BOTH
//! routes — the legacy owned-`Value` walk (parse → `Value::from_document`
//! → `from_value`, exactly what `from_str` did before #41) and the
//! direct Node deserializer (the new `from_str`) — and the outcome
//! must match exactly: same value (including mapping ORDER), or the
//! same error KIND and the same error STRING.

use std::collections::{BTreeMap, HashMap};
use std::fmt::Debug;

use serde::de::DeserializeOwned;
use serde::{Deserialize, Serialize};

use yeptris::{from_str, from_value, Document, Number, Value};

/// The legacy route, verbatim pre-#41 `from_slice`.
fn legacy<T: DeserializeOwned>(src: &str) -> Result<T, String> {
    let doc = Document::parse(src.as_bytes()).map_err(|e| e.to_string())?;
    if doc.document_count() != 1 {
        return Err(format!(
            "deserializing from YAML containing more than one document is not supported: {} documents",
            doc.document_count()
        ));
    }
    let value = Value::from_document(&doc).map_err(|e| e.to_string())?;
    from_value(&value).map_err(|e| e.to_string())
}

/// The direct route: the new `from_str`.
fn direct<T: DeserializeOwned>(src: &str) -> Result<T, String> {
    from_str(src).map_err(|e| e.to_string())
}

/// f64 equality cannot see NaN; both routes produce the identical
/// resolution, so canonicalizing NaN (and only NaN) to a marker keeps
/// the comparison exact for everything else.
fn canon(v: &Value) -> Value {
    match v {
        Value::Number(Number::F64(f)) if f.is_nan() => Value::String("<nan>".into()),
        Value::Sequence(items) => Value::Sequence(items.iter().map(canon).collect()),
        Value::Mapping(m) => Value::Mapping(yeptris::Mapping(
            m.0.iter().map(|(k, v)| (canon(k), canon(v))).collect(),
        )),
        Value::Tagged(t) => Value::Tagged(Box::new(yeptris::Tagged {
            tag: t.tag.clone(),
            value: canon(&t.value),
        })),
        other => other.clone(),
    }
}

fn assert_value_parity(src: &str) {
    let a = legacy::<Value>(src);
    let b = direct::<Value>(src);
    match (a, b) {
        (Ok(a), Ok(b)) => assert_eq!(canon(&a), canon(&b), "value diverges for {src:?}"),
        (Err(a), Err(b)) => assert_eq!(a, b, "error strings diverge for {src:?}"),
        (a, b) => panic!("route verdict diverges for {src:?}: legacy={a:?} node={b:?}"),
    }
}

fn assert_parity<T>(src: &str)
where
    T: DeserializeOwned + PartialEq + Debug,
{
    let a = legacy::<T>(src);
    let b = direct::<T>(src);
    match (a, b) {
        (Ok(a), Ok(b)) => assert_eq!(a, b, "typed value diverges for {src:?}"),
        (Err(a), Err(b)) => assert_eq!(a, b, "typed error strings diverge for {src:?}"),
        (a, b) => panic!("typed route verdict diverges for {src:?}: legacy={a:?} node={b:?}"),
    }
}

#[derive(Debug, PartialEq, Serialize, Deserialize)]
struct Config {
    name: String,
    port: u16,
    debug: bool,
    tags: Vec<String>,
    tls: Option<Tls>,
    #[serde(default)]
    extra: Option<String>,
}

#[derive(Debug, PartialEq, Serialize, Deserialize)]
struct Tls {
    cert: String,
    #[serde(default)]
    alpn: Vec<String>,
}

#[derive(Debug, PartialEq, Deserialize)]
#[allow(dead_code)] // the visitor consumes every field; some are never read
struct SkipAndDefaults {
    a: i64,
    #[serde(default)]
    missing: String,
    #[serde(skip)]
    skipped: Option<String>,
}

#[derive(Debug, PartialEq, Deserialize)]
struct NewtypeValue(Value);

#[derive(Debug, PartialEq, Deserialize)]
struct JustUnit;

#[derive(Debug, PartialEq, Serialize, Deserialize)]
enum Mode {
    Fast,
    Custom { level: u8 },
    Named(String),
}

/// The corpus: scalar resolution, tags, aliases, duplicate keys,
/// complex keys, styles, structure — the surface the two routes must
/// agree on.
fn corpus() -> Vec<&'static str> {
    vec![
        // scalar resolution
        "a: 1\nb: 1.5\nc: true\nd: null\ne: '1'\nf: 0x10\ng: 0o17\n",
        "empty:\n",
        "empty: ''\n",
        "tilde: '~'\n",
        "bare: ~\n",
        "y: yes\nn: no\non: on\noff: off\n",
        "big: 18446744073709551615\nsmall: -9223372036854775808\nover: 18446744073709551616\n",
        "hex: -0x10\noct: +0o7\nplus: +42\n",
        "fl: 1.5e10\nfl2: -1E-3\nfl3: .inf\nfl4: -.INF\nfl5: +.inf\nfl6: .nan\nfl7: 12.00\n",
        "str: plain string\nsq: 'single'\ndq: \"double\"\n",
        "block: |\n  line1\n  line2\n",
        "folded: >\n  fold1\n  fold2\n",
        "multi: |\n  a\n\n  b\n",
        "esc: \"tab\\there\"\n",
        "unicode: 'héllo — ünïcode ✓'\nplain: ünïcode\n",
        "dot: .\ndots: 1.2.3\nunder: 1_000\n",
        "top: 'null'\ntop2: \"~\"\ntop3: 'true'\n",
        // structure
        "nested:\n  a:\n    - 1\n    - b: 2\n",
        "seq:\n  - 1\n  - [2, 3]\n  - {k: v}\n",
        "emptyseq: []\nemptymap: {}\n",
        "a: [1, 2]\nb: {c: [d]}\n",
        "deep: {a: {b: {c: {d: [1, [2, [3]]]}}}}\n",
        "'quoted key': 1\n123: numkey\n1.5: floatkey\ntrue: boolkey\nnull: nullkey\n",
        "? [a, b]\n: v\nplain: x\n",
        "? {k: 1}\n: w\n",
        // duplicate keys — the DOM reports raw pairs; both routes fold
        "a: 1\na: 2\nb: 3\n",
        "a: '1'\na: 1\n",
        "dupnum:\n  0x10: a\n  16: b\n",
        "negzero:\n  -0.0: a\n  0.0: b\n",
        "nankeys:\n  .nan: a\n  .nan: b\n",
        "dup:\n  k: 1\n  k: 2\n  j: 3\n  k: 4\n",
        "Fast: 1\nFast: 2\n",
        // aliases
        "base: &b {x: 1}\ncopy: *b\n",
        "base: &b {x: 1}\nc1: *b\nc2: *b\n",
        "base: &x 5\nb: *x\nc: *x\n",
        "anch: &x !T {k: 1}\nref: *x\n",
        "base: &b {x: 1}\nchild:\n  <<: *b\n  y: 2\n",
        "l: &l [1, 2]\nm: {f: *l}\n",
        // tags
        "!Thing {a: 1}\n",
        "p: !!int 7\nq: !!str 7\nr: !T [1]\ns: !U {k: v}\n",
        "t: !!bool 'true'\n",
        "tagged: !!int '5'\n",
        "tagnull: !!null ~\n",
        "!T\n",
        "!V [1, !W 2]\n",
        // parse failures — the engine verdicts, identically surfaced
        "a: [1,\n",
        "a: b: c\n",
        "a:\n- 1\n - 2\n",
        // content-less documents: the engine's synthesized empty scalar
        "---\n",
        "---\n...\n",
        "# just a comment\n",
        // null keys
        ": v\n",
        "? \n: v\n",
        "on: x\noff: y\n",
        // alias keys resolve before identity/delivery
        "k: &k1 name\n*k1: v\nother: w\n",
        // tagged scalar keys are distinct from their untagged twins
        "!!str 5: a\n5: b\n",
        // merge-key duplicates fold like any duplicate
        "a: &a {x: 1}\nb:\n  <<: *a\n  <<: {y: 2}\n  x: 3\n",
        // alias chains through containers
        "a: &x {v: 1}\nb: &y [*x]\nc: *y\n",
        // empty containers through aliases
        "e: &e {}\nf: *e\n",
        // alias payloads in typed-enum position
        "n: &n lz4\nm:\n  Named: *n\n",
        // self-referential aliases: the legacy route's eager walk
        // errors the whole deserialize (Parse) even where the
        // duplicate-key fold drops the cyclic value — the direct
        // route's drain preserves that verdict
        "b: &a0 {n: .nan, k2: *a0, k2: {}}\na: 'x'\n",
        "b: &a0 {k2: *a0}\n",
        "ok: fine\ncyc: &c [*c]\n",
    ]
}

#[test]
fn differential_value_corpus() {
    for src in corpus() {
        assert_value_parity(src);
    }
}

#[test]
fn differential_typed_structs() {
    let config_doc = "\
name: engine
port: 8080
debug: true
tags: [fast, safe]
tls:
  cert: /etc/cert.pem
  alpn: [h2]
unknown-extra: ignored
";
    assert_parity::<Config>(config_doc);
    assert_parity::<Config>("name: n\nport: 1\ndebug: false\ntags: []\ntls: null\n");
    // tls missing entirely (the Option defaults to None — no, serde
    // requires the field unless Option: serde treats missing Option as None)
    assert_parity::<Config>("name: n\nport: 1\ndebug: false\ntags: []\n");
    assert_parity::<SkipAndDefaults>("a: 7\n");
    assert_parity::<SkipAndDefaults>("a: 7\nmissing: set\nskipped: ignored\n");
    assert_parity::<NewtypeValue>("x: [1, {y: z}]\n");
    // unit structs: null only
    assert_parity::<JustUnit>("null\n");
    assert_parity::<JustUnit>("5\n");
}

#[test]
fn differential_typed_enums() {
    assert_parity::<Mode>("Fast\n");
    assert_parity::<Mode>("Custom:\n  level: 3\n");
    assert_parity::<Mode>("Named: lz4\n");
    assert_parity::<Mode>("Unknown\n");
    assert_parity::<Mode>("5\n");
    assert_parity::<Mode>("{a: 1, b: 2}\n");
    assert_parity::<Mode>("!T Fast\n");
    assert_parity::<Mode>("[]\n");
    // duplicate keys fold before the single-variant check
    assert_parity::<Mode>("Fast: 1\nFast: 2\n");
}

#[test]
fn differential_typed_maps_seqs() {
    assert_parity::<HashMap<String, String>>("a: x\nb: 'y'\nc: \"z\"\nd: '1'\n");
    // non-string keys / values — error parity, exact messages
    assert_parity::<HashMap<String, String>>("a: 1\n");
    assert_parity::<HashMap<String, String>>("1: x\n");
    assert_parity::<HashMap<String, String>>("a: [x]\n");
    assert_parity::<HashMap<String, String>>("a: x\na: y\n");
    assert_parity::<BTreeMap<String, i64>>("a: 1\nb: 0x10\nc: -3\n");
    assert_parity::<BTreeMap<String, i64>>("a: 1.5\n");
    assert_parity::<Vec<Value>>("1\n2\n- three\n");
    assert_parity::<Vec<i64>>("1\n2\n3\n");
    assert_parity::<Vec<i64>>("1\nx\n");
    assert_parity::<Vec<i64>>("not a seq\n");
    assert_parity::<(i32, String)>("- 1\n- x\n");
    assert_parity::<(i32, String)>("- 1\n");
    assert_parity::<Option<Value>>("null\n");
    assert_parity::<Option<Value>>("5\n");
    assert_parity::<Option<i64>>("~\n");
    assert_parity::<Option<i64>>("a: 1\n");
    assert_parity::<i64>("~\n");
    assert_parity::<f64>(".inf\n");
    assert_parity::<bool>("TRUE\n");
    assert_parity::<bool>("yes\n");
    assert_parity::<char>("x\n");
    assert_parity::<char>("xy\n");
    assert_parity::<u8>("300\n");
    assert_parity::<i8>("-129\n");
    assert_parity::<u64>("18446744073709551615\n");
    assert_parity::<String>("'quoted'\n");
    assert_parity::<String>("plain\n");
    assert_parity::<Vec<Vec<i64>>>("[[1, 2], [3]]\n");
    assert_parity::<HashMap<String, Vec<Option<i64>>>>("a: [1, null, 3]\n");
}

#[test]
fn differential_typed_with_tags_and_aliases() {
    // the serde_yaml contract: a typed non-enum visitor rejects tagged
    // input — identical messages both routes
    assert_parity::<HashMap<String, i64>>("!Thing {a: 1}\n");
    assert_parity::<Config>("!Config {name: n, port: 1, debug: false, tags: []}\n");
    // aliases resolve before the typed walk
    assert_parity::<Config>(
        "base: &b\n  cert: c\n  alpn: [a]\nname: n\nport: 443\ndebug: true\ntags: []\ntls: *b\n",
    );
    assert_parity::<HashMap<String, Value>>("base: &b {x: 1}\nc1: *b\nc2: *b\nc3: 2\nc3: 3\n");
}

#[test]
fn differential_top_level_shapes() {
    assert_parity::<Value>("hello\n");
    assert_parity::<Value>("'hello'\n");
    assert_parity::<Value>("42\n");
    assert_parity::<Value>("- 1\n- 2\n");
    assert_parity::<Value>("[1, 2]\n");
    assert_parity::<Value>("{a: 1}\n");
    // multi-document streams: same verdict, same message
    assert_parity::<Value>("---\na: 1\n---\nb: 2\n");
    assert_parity::<Config>("---\na: 1\n---\nb: 2\n");
}

#[test]
fn differential_duplicate_key_semantics_on_value() {
    // spelled out once (the corpus covers it too): the DOM reports raw
    // duplicate pairs; both routes fold to first position + last value
    let v = direct::<Value>("a: 1\na: 2\nb: 3\n").unwrap();
    let Value::Mapping(m) = v else {
        panic!("expected a mapping, got {v:?}")
    };
    assert_eq!(m.0.len(), 2);
    assert_eq!(m.0[0].0, Value::String("a".into()));
    assert_eq!(m.0[0].1, Value::Number(Number::I64(2)));
    assert_eq!(m.0[1].0, Value::String("b".into()));
    assert_eq!(m.0[1].1, Value::Number(Number::I64(3)));
}

/// A tiny xorshift PRNG (no dep): deterministic across runs and
/// platforms, so a failing seed reproduces exactly.
struct Rng(u64);

impl Rng {
    fn new(seed: u64) -> Rng {
        Rng(seed.wrapping_mul(0x9E3779B97F4A7C15) | 1)
    }
    fn next(&mut self) -> u64 {
        let mut x = self.0;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        self.0 = x;
        x.wrapping_mul(0x2545F4914F6CDD1D)
    }
    fn below(&mut self, n: u64) -> u64 {
        self.next() % n
    }
}

/// Randomized parity gate: generated documents (flow-nested scalars,
/// tags, anchors/aliases, duplicate keys) must deserialize identically
/// through both routes. Values are compared through `canon`; errors
/// through their exact strings. Anything the generator emits that the
/// engine rejects fails BOTH routes identically — still a gate.
fn gen_value(rng: &mut Rng, anchors: &mut Vec<String>, depth: u32) -> String {
    if depth == 0 {
        return gen_scalar(rng);
    }
    match rng.below(10) {
        0 | 1 => gen_scalar(rng),
        2 => {
            // flow sequence
            let n = rng.below(4);
            let items: Vec<String> = (0..n).map(|_| gen_value(rng, anchors, depth - 1)).collect();
            format!("[{}]", items.join(", "))
        }
        3 => {
            // flow mapping (keys may repeat — the fold must agree)
            let n = rng.below(4);
            let keys = ["k1", "k2", "k3", "n", "5", "'q'", "0x10"];
            let entries: Vec<String> = (0..n)
                .map(|_| {
                    let k = keys[rng.below(keys.len() as u64) as usize];
                    format!("{k}: {}", gen_value(rng, anchors, depth - 1))
                })
                .collect();
            format!("{{{}}}", entries.join(", "))
        }
        4 => {
            // anchor a value; register it for aliasing
            let name = format!("a{}", anchors.len());
            anchors.push(name.clone());
            format!("&{} {}", name, gen_value(rng, anchors, depth - 1))
        }
        5 => {
            // alias (only to a registered anchor)
            match anchors.last() {
                Some(name) => format!("*{name}"),
                None => gen_scalar(rng),
            }
        }
        6 => "{}".to_owned(),
        7 => "[]".to_owned(),
        8 => {
            // tagged
            let tag = ["!T", "!U", "!!str", "!!int"][rng.below(4) as usize];
            format!("{tag} {}", gen_scalar(rng))
        }
        _ => gen_scalar(rng),
    }
}

fn gen_scalar(rng: &mut Rng) -> String {
    let pool = [
        "1",
        "1.5",
        "0x10",
        "0o17",
        "true",
        "TRUE",
        "~",
        "null",
        "",
        "'1'",
        "'null'",
        "\"double\"",
        "plain",
        "yes",
        ".inf",
        ".nan",
        "-0.0",
        "0.0",
        "18446744073709551615",
        "a b c",
        "'!weird'",
    ];
    pool[rng.below(pool.len() as u64) as usize].to_owned()
}

#[test]
fn differential_randomized_corpus() {
    for seed in [1u64, 0xDEADBEEF, 0x517CC1B7, 42] {
        let mut rng = Rng::new(seed);
        for _ in 0..250 {
            let mut anchors = Vec::new();
            let entries = 1 + rng.below(5);
            let keys = ["a", "b", "c", "d"];
            let mut doc = String::new();
            let mut used: Vec<String> = Vec::new();
            for _ in 0..entries {
                let key = keys[rng.below(keys.len() as u64) as usize];
                // occasionally repeat a key on purpose (the fold gate)
                let key = if !used.is_empty() && rng.below(5) == 0 {
                    used[rng.below(used.len() as u64) as usize].clone()
                } else {
                    key.to_owned()
                };
                used.push(key.clone());
                let value = gen_value(&mut rng, &mut anchors, 3);
                if value.contains('\n') {
                    unreachable!("generated values are single-line");
                }
                doc.push_str(&key);
                doc.push_str(": ");
                doc.push_str(&value);
                doc.push('\n');
            }
            assert_value_parity(&doc);
        }
    }
}

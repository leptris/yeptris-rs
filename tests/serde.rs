//! The serde face: `Value` fidelity (the serde_yaml data model),
//! `from_str`/`to_string` round-trips, and the documented divergences.

use serde::{Deserialize, Serialize};
use yeptris::{from_str, from_value, to_string, to_value, Mapping, Number, Tagged, Value};

#[derive(Debug, PartialEq, Serialize, Deserialize)]
struct Config {
    name: String,
    port: u16,
    debug: bool,
    tags: Vec<String>,
    tls: Option<Tls>,
}

#[derive(Debug, PartialEq, Serialize, Deserialize)]
struct Tls {
    cert: String,
    #[serde(default)]
    alpn: Vec<String>,
}

#[test]
fn typed_struct_round_trip() {
    let src = "\
name: engine
port: 8080
debug: true
tags: [fast, safe]
tls:
  cert: /etc/cert.pem
  alpn: [h2]
";
    let cfg: Config = from_str(src).unwrap();
    assert_eq!(cfg.port, 8080);
    assert_eq!(cfg.tags, ["fast", "safe"]);
    assert_eq!(cfg.tls.as_ref().unwrap().alpn, ["h2"]);

    let dumped = to_string(
        &Value::from_document(&yeptris::Document::parse(src.as_bytes()).unwrap()).unwrap(),
    )
    .unwrap();
    let back: Config = from_str(&dumped).unwrap();
    assert_eq!(back, cfg);

    // struct → Value → YAML → struct
    let v = to_value(&cfg).unwrap();
    let yaml = to_string(&v).unwrap();
    let back2: Config = from_str(&yaml).unwrap();
    assert_eq!(back2, cfg);
}

#[test]
fn scalar_resolution() {
    let v: Value = from_str("a: 1\nb: 1.5\nc: true\nd: null\ne: '1'\nf: 0x10\ng: 0o17\n").unwrap();
    let m = match &v {
        Value::Mapping(m) => m,
        other => panic!("{:?}", other),
    };
    assert_eq!(
        m.get(&Value::String("a".into())),
        Some(&Value::Number(Number::I64(1)))
    );
    assert_eq!(
        m.get(&Value::String("b".into())),
        Some(&Value::Number(Number::F64(1.5)))
    );
    assert_eq!(m.get(&Value::String("c".into())), Some(&Value::Bool(true)));
    assert_eq!(m.get(&Value::String("d".into())), Some(&Value::Null));
    assert_eq!(
        m.get(&Value::String("e".into())),
        Some(&Value::String("1".into()))
    );
    assert_eq!(
        m.get(&Value::String("f".into())),
        Some(&Value::Number(Number::I64(16)))
    );
    assert_eq!(
        m.get(&Value::String("g".into())),
        Some(&Value::Number(Number::I64(15)))
    );
}

#[test]
fn tags_are_kept_then_unwrapped_for_typed() {
    let v: Value = from_str("!Thing {a: 1}").unwrap();
    match &v {
        Value::Tagged(t) => {
            assert!(t.tag.contains("Thing"));
            assert!(matches!(t.value, Value::Mapping(_)));
        }
        other => panic!("{:?}", other),
    }
    // the serde_yaml contract: tags are preserved for Value; a typed
    // non-enum visitor rejects tagged input (TAGS-ARE-VALUE-ONLY)
    let typed = from_str::<std::collections::BTreeMap<String, i64>>("!Thing {a: 1}");
    assert!(typed.is_err());
}

#[test]
fn aliases_expand() {
    let v: Value = from_str("base: &b {x: 1}\ncopy: *b\n").unwrap();
    let m = match &v {
        Value::Mapping(m) => m,
        _ => panic!(),
    };
    let base = m.get(&Value::String("base".into())).unwrap();
    let copy = m.get(&Value::String("copy".into())).unwrap();
    assert_eq!(base, copy);
}

#[test]
fn merge_key_stays_literal_yaml12() {
    // documented divergence from serde_yaml/Psych: YAML 1.2 removed
    // merge keys, the engine keeps `<<` literal
    let v: Value = from_str("base: &b {x: 1}\nchild:\n  <<: *b\n  y: 2\n").unwrap();
    let child = match &v {
        Value::Mapping(m) => m.get(&Value::String("child".into())).unwrap().clone(),
        _ => panic!(),
    };
    let cm = match child {
        Value::Mapping(m) => m,
        _ => panic!(),
    };
    assert!(cm.get(&Value::String("<<".into())).is_some());
    assert!(cm.get(&Value::String("x".into())).is_none());
}

#[test]
fn multi_document_is_an_error() {
    let e = from_str::<Value>("---\na: 1\n---\nb: 2\n").unwrap_err();
    assert!(e.to_string().contains("more than one document"), "{}", e);
}

#[test]
fn dump_quotes_resolvable_strings() {
    let mut m = Mapping::new();
    m.insert(Value::String("n".into()), Value::String("1e0".into()));
    m.insert(Value::String("y".into()), Value::String("yes".into()));
    m.insert(
        Value::String("plain".into()),
        Value::String("hello world".into()),
    );
    let text = to_string(&Value::Mapping(m)).unwrap();
    assert!(text.contains("\"1e0\""), "{}", text);
    // "yes" is a plain string under YAML 1.2 (only true/false are bools)
    assert!(text.contains("yes"), "{}", text);
    assert!(text.contains("hello world"), "{}", text);
    let back: Value = from_str(&text).unwrap();
    let rm = match back {
        Value::Mapping(m) => m,
        _ => panic!(),
    };
    assert_eq!(
        rm.get(&Value::String("n".into())),
        Some(&Value::String("1e0".into()))
    );
}

#[test]
fn value_round_trip_preserves_bytes() {
    let src = "a: 1\nb: [x, y]\nc:\n  d: 1.5\n";
    let v: Value = from_str(src).unwrap();
    let out = to_string(&v).unwrap();
    let v2: Value = from_str(&out).unwrap();
    assert_eq!(v, v2);
}

#[test]
fn enum_externally_tagged() {
    #[derive(Debug, PartialEq, Serialize, Deserialize)]
    enum Mode {
        Fast,
        Custom { level: u8 },
        Named(String),
    }
    assert_eq!(from_str::<Mode>("Fast").unwrap(), Mode::Fast);
    assert_eq!(
        from_str::<Mode>("Custom:\n  level: 3").unwrap(),
        Mode::Custom { level: 3 }
    );
    assert_eq!(
        from_str::<Mode>("Named: lz4").unwrap(),
        Mode::Named("lz4".into())
    );
}

#[test]
fn tagged_value_round_trips_through_value() {
    let tagged = Value::Tagged(Box::new(Tagged {
        tag: "!T".into(),
        value: Value::Number(Number::I64(7)),
    }));
    // Value's own Deserialize rebuilds the tag through the enum path
    let back: Value = from_value(&tagged).unwrap();
    assert_eq!(back, tagged);
}

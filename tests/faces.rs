//! The JSON and CBOR faces (yeptris-rs#17) — parity with the py and
//! Ruby bindings: one document model, three serializations.

use serde::{Deserialize, Serialize};

#[derive(Debug, PartialEq, Serialize, Deserialize)]
struct Repo {
    name: String,
    langs: Vec<String>,
    stars: u64,
    ratio: f64,
    archived: bool,
    parent: Option<String>,
}

fn sample() -> Repo {
    Repo {
        name: "yeptris".into(),
        langs: vec!["c".into(), "rust".into()],
        stars: 42,
        ratio: 3.5,
        archived: false,
        parent: None,
    }
}

#[test]
fn json_struct_round_trip() {
    let r = sample();
    let text = yeptris::json::to_string(&r).unwrap();
    let back: Repo = yeptris::json::from_str(&text).unwrap();
    assert_eq!(back, r);
}

#[test]
fn json_parse_and_serialize_canonical() {
    let v = yeptris::json::parse(r#"{"b": [1, 2], "a": null}"#).unwrap();
    let out = yeptris::json::serialize(&v).unwrap();
    assert_eq!(
        out,
        r#"{"b": [1, 2], "a": null}
"#
    );
}

#[test]
fn json_document_level_apis() {
    let doc = yeptris::Document::parse_json(br#"{"k": [true, 1.5]}"#).unwrap();
    assert_eq!(doc.document_count(), 1);
    let bytes = doc.serialize_json().unwrap();
    assert_eq!(
        String::from_utf8(bytes).unwrap(),
        r#"{"k": [true, 1.5]}
"#
    );
}

#[test]
fn json_malformed_carries_the_error() {
    assert!(yeptris::Document::parse_json(br#"{"k": [1,}"#).is_err());
}

#[test]
fn cbor_known_encodings() {
    // RFC 8949 appendix A values through the engine face.
    assert_eq!(
        yeptris::cbor::encode(&yeptris::Value::Null).unwrap(),
        vec![0xf6]
    );
    assert_eq!(
        yeptris::cbor::encode(&yeptris::Value::Bool(true)).unwrap(),
        vec![0xf5]
    );
    assert_eq!(
        yeptris::cbor::encode(&yeptris::Value::Number(yeptris::Number::U64(1))).unwrap(),
        vec![0x01]
    );
    assert_eq!(
        yeptris::cbor::encode(&yeptris::Value::Number(yeptris::Number::I64(-5))).unwrap(),
        vec![0x24]
    );
    assert_eq!(
        yeptris::cbor::encode(&yeptris::Value::String("a".into())).unwrap(),
        vec![0x61, b'a']
    );
}

#[test]
fn cbor_struct_round_trip() {
    let r = sample();
    let bytes = yeptris::cbor::to_vec(&r).unwrap();
    let back: Repo = yeptris::cbor::from_slice(&bytes).unwrap();
    assert_eq!(back, r);
}

#[test]
fn cbor_document_level_apis() {
    // [1, 2] = 0x82 0x01 0x02
    let doc = yeptris::Document::parse_cbor(&[0x82, 0x01, 0x02]).unwrap();
    let back = doc.serialize_cbor().unwrap();
    assert_eq!(back, vec![0x82, 0x01, 0x02]);
}

#[test]
fn cbor_malformed_carries_the_error() {
    // a definite-length string header claiming 9 bytes with none
    assert!(yeptris::Document::parse_cbor(&[0x69]).is_err());
}

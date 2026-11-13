//! An explicit core `!!str` tag fixes the node's type: the scalar's
//! source text is the value. YAML 1.2 core-schema resolution applies
//! only to untagged plain scalars, so `!!str 0123` must keep `0123`
//! and must not be resolved to the integer 123 first.

use yeptris::{from_str, from_value, parse_stream, Document, Mapping, Number, Value};

const STR_TAG: &str = "tag:yaml.org,2002:str";

/// (source scalar after the tag, expected string value)
const CASES: &[(&str, &str)] = &[
    ("123", "123"),
    ("0123", "0123"),
    ("-007", "-007"),
    ("0x1F", "0x1F"),
    ("0o17", "0o17"),
    ("1.50", "1.50"),
    ("1e3", "1e3"),
    (".inf", ".inf"),
    (".nan", ".nan"),
    ("true", "true"),
    ("False", "False"),
    ("null", "null"),
    ("~", "~"),
];

fn str_tagged(text: &str) -> Value {
    Value::Tagged(Box::new(yeptris::Tagged {
        tag: STR_TAG.to_owned(),
        value: Value::String(text.to_owned()),
    }))
}

fn field<'a>(v: &'a Value, key: &str) -> &'a Value {
    match v {
        Value::Mapping(m) => m
            .get(&Value::String(key.to_owned()))
            .unwrap_or_else(|| panic!("missing key {key:?} in {v:?}")),
        other => panic!("expected a mapping, got {other:?}"),
    }
}

/// Every Value route: the DOM walk, the direct serde route and the
/// stream face.
fn all_routes(src: &str) -> Vec<Value> {
    let doc = Document::parse(src.as_bytes()).unwrap();
    let dom = Value::from_document(&doc).unwrap();
    let direct: Value = from_str(src).unwrap();
    let mut stream = parse_stream(src.as_bytes()).unwrap();
    assert_eq!(stream.len(), 1, "{src:?}");
    vec![dom, direct, stream.remove(0)]
}

#[test]
fn explicit_str_tag_keeps_scalar_text() {
    for (text, expected) in CASES {
        let src = format!("n: !!str {text}\n");
        for v in all_routes(&src) {
            let n = field(&v, "n");
            assert_eq!(n, &str_tagged(expected), "{src:?}");
            // ordinary serde String consumer of the tagged payload
            let Value::Tagged(t) = n else { unreachable!() };
            let s: String = from_value(&t.value).unwrap();
            assert_eq!(s, *expected, "{src:?}");
        }
    }
}

#[test]
fn explicit_str_tag_root_and_sequence_items() {
    for v in all_routes("!!str 0123\n") {
        assert_eq!(v, str_tagged("0123"));
    }
    for v in all_routes("- !!str 0123\n- !!str 1.50\n- !!str true\n") {
        assert_eq!(
            v,
            Value::Sequence(vec![
                str_tagged("0123"),
                str_tagged("1.50"),
                str_tagged("true"),
            ])
        );
    }
}

#[test]
fn verbatim_core_str_tag_keeps_scalar_text() {
    for v in all_routes("n: !<tag:yaml.org,2002:str> 0123\n") {
        assert_eq!(field(&v, "n"), &str_tagged("0123"));
    }
}

#[test]
fn empty_explicit_str_is_the_empty_string() {
    for v in all_routes("n: !!str\nm: x\n") {
        assert_eq!(field(&v, "n"), &str_tagged(""));
    }
}

#[test]
fn untagged_and_other_tags_resolve_as_before() {
    let src = "a: 123\nb: 0123\nc: '0123'\nd: \"1.50\"\ne: !!int 7\nf: true\ng: ~\nh: !T 5\n";
    for v in all_routes(src) {
        assert_eq!(field(&v, "a"), &Value::Number(Number::I64(123)));
        assert!(
            matches!(field(&v, "b"), Value::Number(_)),
            "untagged 0123 stays numeric: {v:?}"
        );
        assert_eq!(field(&v, "c"), &Value::String("0123".into()));
        assert_eq!(field(&v, "d"), &Value::String("1.50".into()));
        assert_eq!(
            field(&v, "e"),
            &Value::Tagged(Box::new(yeptris::Tagged {
                tag: "tag:yaml.org,2002:int".into(),
                value: Value::Number(Number::I64(7)),
            }))
        );
        assert_eq!(field(&v, "f"), &Value::Bool(true));
        assert_eq!(field(&v, "g"), &Value::Null);
        match field(&v, "h") {
            Value::Tagged(t) => {
                assert!(t.tag.ends_with('T'), "{t:?}");
                assert_eq!(t.value, Value::Number(Number::I64(5)));
            }
            other => panic!("expected a local-tagged number, got {other:?}"),
        }
    }
}

#[test]
fn quoted_explicit_str_is_unchanged() {
    for v in all_routes("n: !!str '0123'\nm: !!str \"1.50\"\n") {
        assert_eq!(field(&v, "n"), &str_tagged("0123"));
        assert_eq!(field(&v, "m"), &str_tagged("1.50"));
    }
}

#[test]
fn str_tagged_key_stays_distinct_from_untagged_twin() {
    for v in all_routes("!!str 05: a\n5: b\n") {
        let Value::Mapping(Mapping(pairs)) = v else {
            panic!("expected a mapping")
        };
        assert_eq!(pairs.len(), 2);
        assert_eq!(pairs[0].0, str_tagged("05"));
        assert_eq!(pairs[1].0, Value::Number(Number::I64(5)));
    }
}

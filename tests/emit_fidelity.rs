//! Float scalar fidelity on the dump lane and the explicit emitter
//! options face (`EmitOptions` over `yeptris_serialize_ex`).
//!
//! Floats: `to_string` must spell an `f64` so it re-parses as a float
//! with the identical bit pattern, in the engine's own shortest-float
//! byte shape. Before the fix, `1.0` dumped as `1` and re-parsed as
//! the int `1` (a register fixture's `range: [0.0, 1.0]` came back as
//! `[0, 1]`), and `f64::INFINITY` dumped as `inf`, a string.

#![cfg(feature = "serde")]

use yeptris::{
    from_str, to_string, to_string_with, Document, EmitOptions, ErrorKind, LineWidth, Mapping,
    Number, Value,
};

fn f(x: f64) -> Value {
    Value::Number(Number::F64(x))
}

/// Dump, re-parse, and demand a float with the same bits (NaN: any
/// NaN). Returns the dumped text.
fn roundtrip_float(x: f64) -> String {
    let text = to_string(&f(x)).unwrap();
    match from_str::<Value>(&text).unwrap() {
        Value::Number(Number::F64(y)) => {
            if x.is_nan() {
                assert!(y.is_nan(), "{x:?} dumped as {text:?} re-read as {y:?}");
            } else {
                assert_eq!(
                    y.to_bits(),
                    x.to_bits(),
                    "{x:?} dumped as {text:?} re-read as {y:?}"
                );
            }
        }
        other => panic!("{x:?} dumped as {text:?} re-read as non-float {other:?}"),
    }
    text
}

#[test]
fn float_spellings_match_the_engine_shortest_shape() {
    let cases: &[(f64, &str)] = &[
        (0.0, "0.0"),
        (-0.0, "-0.0"),
        (1.0, "1.0"),
        (-1.0, "-1.0"),
        (1.5, "1.5"),
        (0.1, "0.1"),
        (0.30000000000000004, "0.30000000000000004"),
        (100.0, "100.0"),
        (123.25, "123.25"),
        (0.0001, "0.0001"),
        (0.00012, "0.00012"),
        (1e15, "1000000000000000.0"),
        (9007199254740993.0, "9007199254740992.0"),
        (1e16, "1.0e+16"),
        (1.5e300, "1.5e+300"),
        (1e-5, "1.0e-05"),
        (1.25e-7, "1.25e-07"),
        (5e-324, "5.0e-324"),
        (f64::MAX, "1.7976931348623157e+308"),
        (f64::MIN_POSITIVE, "2.2250738585072014e-308"),
        (f64::INFINITY, ".inf"),
        (f64::NEG_INFINITY, "-.inf"),
        (f64::NAN, ".nan"),
    ];
    for &(x, want) in cases {
        assert_eq!(roundtrip_float(x), want, "spelling of {x:?}");
    }
}

#[test]
fn float_spelling_is_the_engine_canonical_reprint() {
    // the engine's canonical mode re-prints every float through its
    // own shortest printer (writer.c emit_canonical_scalar): the dump
    // lane's text must be a fixed point of that printer
    let canonical = EmitOptions::new().canonical(true);
    for x in [
        0.0,
        -0.0,
        1.0,
        0.5,
        2.5e-5,
        1e15,
        1e16,
        3.0e-4,
        6.02214076e23,
        1e-310,
        f64::MAX,
    ] {
        let text = to_string(&f(x)).unwrap();
        let src = format!("- {text}\n");
        let doc = Document::parse(src.as_bytes()).unwrap();
        let out = doc.serialize_string_with(&canonical).unwrap();
        assert!(
            out.contains(&text),
            "engine canonical re-print {out:?} does not carry {text:?} for {x:?}"
        );
    }
}

#[test]
fn float_bit_patterns_roundtrip() {
    // a deterministic sweep over raw bit patterns (every exponent
    // band, subnormals and signs) — no external rand dependency
    let mut state: u64 = 0x9e37_79b9_7f4a_7c15;
    for _ in 0..20_000 {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        let x = f64::from_bits(state);
        roundtrip_float(x);
    }
    // integral-valued floats across the plain/scientific boundary
    let mut p = 1.0f64;
    for _ in 0..40 {
        roundtrip_float(p);
        roundtrip_float(-p);
        roundtrip_float(p + 1.0);
        p *= 10.0;
    }
}

#[test]
fn integral_floats_keep_their_type_inside_collections() {
    // the register-fixture shape that exposed the defect
    let src = "range: [0.0, 1.0]\nscale: 2.0\ncount: 2\n";
    let before: Value = from_str(src).unwrap();
    let text = to_string(&before).unwrap();
    let after: Value = from_str(&text).unwrap();
    assert_eq!(after, before, "dumped as {text:?}");
    let Value::Mapping(m) = &after else {
        panic!("not a mapping: {after:?}")
    };
    let range = m.get(&Value::String("range".into())).unwrap();
    assert_eq!(range, &Value::Sequence(vec![f(0.0), f(1.0)]));
    assert_eq!(
        m.get(&Value::String("count".into())),
        Some(&Value::Number(Number::I64(2)))
    );
}

// ---- EmitOptions ----

const LONG: &str = "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi \
                    omicron pi rho sigma tau upsilon phi chi psi omega";

fn long_doc() -> Document<'static> {
    let src = format!("description: {LONG}\nitems:\n  - {LONG}\n  - short\n");
    let doc = Document::parse(src.as_bytes()).unwrap();
    // re-own: parse borrows the input
    let bytes = doc.serialize().unwrap();
    Document::parse(Box::leak(bytes.into_boxed_slice())).unwrap()
}

#[test]
fn default_options_write_the_default_bytes() {
    let inputs: &[&[u8]] = &[
        b"a: 1\nb: [x, y]\n",
        b"--- a\n--- b\n",
        b"k: |\n  line1\n  line2\n",
        b"f: 1.0\ns: 'quoted'\n",
    ];
    for src in inputs {
        let doc = Document::parse(src).unwrap();
        assert_eq!(
            doc.serialize_with(&EmitOptions::new()).unwrap(),
            doc.serialize().unwrap()
        );
        assert_eq!(
            doc.serialize_with(&EmitOptions::default()).unwrap(),
            doc.serialize().unwrap()
        );
    }
    let doc = long_doc();
    assert_eq!(
        doc.serialize_with(&EmitOptions::new()).unwrap(),
        doc.serialize().unwrap()
    );
}

#[test]
fn unbounded_unfolded_keeps_long_scalars_on_one_line() {
    let doc = long_doc();
    let folded = doc.serialize_string().unwrap();
    // libyaml/Psych fold law: the break lands at the first interior
    // space PAST the width, so a line may overrun by one word — the
    // check is that the long scalars fold at all
    assert!(
        !folded.contains(LONG),
        "default should fold at 80: {folded:?}"
    );
    let opts = EmitOptions::new()
        .line_width(LineWidth::Unbounded)
        .block_fold(false);
    let single = doc.serialize_string_with(&opts).unwrap();
    assert!(
        single.contains(&format!("description: {LONG}\n")),
        "{single:?}"
    );
    assert!(single.contains(&format!("- {LONG}\n")), "{single:?}");
    // repeatable, and a fixed point under the same settings
    assert_eq!(doc.serialize_string_with(&opts).unwrap(), single);
    let again = Document::parse(single.as_bytes()).unwrap();
    assert_eq!(again.serialize_string_with(&opts).unwrap(), single);
    // either knob alone also unwraps (the engine's documented law)
    for alone in [
        EmitOptions::new().line_width(LineWidth::Unbounded),
        EmitOptions::new().block_fold(false),
    ] {
        assert_eq!(doc.serialize_string_with(&alone).unwrap(), single);
    }
    // values survive the unwrap
    let a: Value = from_str(&folded).unwrap();
    let b: Value = from_str(&single).unwrap();
    assert_eq!(a, b);
}

#[test]
fn explicit_columns_fold_narrower() {
    let doc = long_doc();
    let opts = EmitOptions::new().line_width(LineWidth::Columns(40));
    let out = doc.serialize_string_with(&opts).unwrap();
    // folds past column 40 (one-word overrun allowed, as above):
    // strictly more lines than the 80-column default
    let default = doc.serialize_string().unwrap();
    assert!(
        out.lines().count() > default.lines().count(),
        "{out:?} vs {default:?}"
    );
    assert!(
        out.lines()
            .all(|l| l.len() <= 40 + LONG.split(' ').map(str::len).max().unwrap() + 1),
        "{out:?}"
    );
    let a: Value = from_str(&out).unwrap();
    let b: Value = from_str(&doc.serialize_string().unwrap()).unwrap();
    assert_eq!(a, b);
}

#[test]
fn indent_and_document_start() {
    let doc = Document::parse(b"a:\n  b: [1, 2]\n  c:\n    - x\n").unwrap();
    let out = doc
        .serialize_string_with(&EmitOptions::new().indent(4).explicit_document_start(true))
        .unwrap();
    assert!(out.starts_with("---"), "{out:?}");
    assert!(out.contains("\n    b:"), "{out:?}");
    let a: Value = from_str(&out).unwrap();
    let b: Value = from_str(&doc.serialize_string().unwrap()).unwrap();
    assert_eq!(a, b);
}

#[test]
fn canonical_floats_and_types_survive() {
    let doc = Document::parse(b"r: [0.0, 1.0, 1e3]\nn: 7\n").unwrap();
    let out = doc
        .serialize_string_with(&EmitOptions::new().canonical(true))
        .unwrap();
    let v: Value = from_str(&out).unwrap();
    let mut want = Mapping::new();
    want.insert(
        Value::String("r".into()),
        Value::Sequence(vec![f(0.0), f(1.0), f(1000.0)]),
    );
    want.insert(Value::String("n".into()), Value::Number(Number::I64(7)));
    assert_eq!(v, Value::Mapping(want), "{out:?}");
}

#[test]
fn out_of_range_settings_are_refused() {
    let doc = Document::parse(b"a: 1\n").unwrap();
    let bad = [
        EmitOptions::new().indent(0),
        EmitOptions::new().indent(1),
        EmitOptions::new().indent(10),
        EmitOptions::new().line_width(LineWidth::Columns(0)),
        EmitOptions::new().line_width(LineWidth::Columns(4)),
        EmitOptions::new()
            .indent(9)
            .line_width(LineWidth::Columns(18)),
        EmitOptions::new().line_width(LineWidth::Columns(u32::MAX)),
    ];
    for opts in bad {
        let err = doc.serialize_with(&opts).unwrap_err();
        assert_eq!(err.kind(), ErrorKind::Arg, "{opts:?}");
        let err = to_string_with(&Value::Null, &opts).unwrap_err();
        assert_eq!(err.kind(), ErrorKind::Arg, "{opts:?}");
    }
    // the boundary just inside the range is accepted
    doc.serialize_with(
        &EmitOptions::new()
            .indent(9)
            .line_width(LineWidth::Columns(19)),
    )
    .unwrap();
    doc.serialize_with(&EmitOptions::new().line_width(LineWidth::Columns(5)))
        .unwrap();
}

#[test]
fn unrepresentable_document_maps_to_not_representable() {
    let doc = Document::parse(b"{k: &a?b v}").unwrap();
    let opts = EmitOptions::new()
        .line_width(LineWidth::Unbounded)
        .block_fold(false);
    let err = doc.serialize_with(&opts).unwrap_err();
    assert_eq!(err.kind(), ErrorKind::NotRepresentable);
}

#[test]
fn to_string_with_matches_to_string_by_default() {
    let v: Value = from_str(&format!("k: {LONG}\nf: [1.0, -0.0, .inf]\n")).unwrap();
    assert_eq!(
        to_string_with(&v, &EmitOptions::new()).unwrap(),
        to_string(&v).unwrap()
    );
    let single = to_string_with(
        &v,
        &EmitOptions::new()
            .line_width(LineWidth::Unbounded)
            .block_fold(false),
    )
    .unwrap();
    assert!(single.contains(&format!("k: {LONG}")), "{single:?}");
    assert!(!single.ends_with('\n'));
    let back: Value = from_str(&single).unwrap();
    assert_eq!(back, v);
}

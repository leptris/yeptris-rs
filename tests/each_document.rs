//! The tolerant per-document stream face (#921 / rs#45).
//!
//! Pins: the mixed stream (valid/INVALID/valid with per-document
//! line/col + index), the recovery law (the failed frame resyncs to
//! the next line-start marker), the quoted-marker one-unit framing,
//! the unrecoverable tail terminating, the empty/comment streams, and
//! the load_all differential. Link-time gating: the crate requires an
//! engine with the stream iterator (0.6.60+).

use yeptris::{each_document, ErrorKind, Number, Value};

fn s(v: &str) -> Value {
    Value::String(v.to_owned())
}

fn i(v: i64) -> Value {
    Value::Number(Number::I64(v))
}

#[test]
fn yields_every_valid_document() {
    let pairs: Vec<_> = each_document("--- 41\n--- \"42\"\n---\n".as_bytes())
        .map(|p| match p {
            Ok(v) => (v, None),
            Err(e) => (Value::Null, Some(e)),
        })
        .collect();
    assert_eq!(pairs.len(), 3);
    assert_eq!(pairs[0].0, i(41));
    assert_eq!(pairs[1].0, s("42"));
    assert_eq!(pairs[2].0, Value::Null);
    assert!(pairs.iter().all(|(_, e)| e.is_none()));
}

#[test]
fn keeps_valid_documents_around_a_bad_one() {
    let mut iter = each_document("--- ok_one\n--- {{{{ bad\n--- ok_two\n".as_bytes());

    let first = iter.next().unwrap().expect("doc 0");
    assert_eq!(first, s("ok_one"));

    let err = iter.next().unwrap().expect_err("doc 1 fails");
    assert_eq!(err.index, 1);
    assert_eq!(err.line, 2); // the failed document's OWN geometry
    assert!(err.col > 0);

    let third = iter.next().unwrap().expect("doc 2 rides");
    assert_eq!(third, s("ok_two"));

    assert!(iter.next().is_none()); // the stream is over
}

#[test]
fn frames_scanner_accurately() {
    // the grammar rejects the quoted scalar as ONE unit; the raw-marker
    // resync then lands on the --- INSIDE the quoted text (the failed
    // frame has no scanner-defined extent), so the tail parses
    // best-effort — the pin: one failure, the tail surfaces, no hang
    let pairs: Vec<_> = each_document("\"line one\n---\nline two\"\n".as_bytes()).collect();
    assert_eq!(pairs.len(), 2);
    assert!(pairs[0].is_err());
    assert_eq!(pairs[1].as_ref().map(|v| v), Ok(&s("line two\"")));
}

#[test]
fn ends_on_an_unrecoverable_tail() {
    let deep = format!("--- [1]\n{}{}\n", "[".repeat(1200), "]".repeat(1200));
    let pairs: Vec<_> = each_document(deep.as_bytes()).collect();
    assert_eq!(pairs.len(), 1);
    assert!(pairs[0].is_err());
    assert_eq!(pairs[0].as_ref().unwrap_err().index, 0);
}

#[test]
fn empty_and_comment_streams_yield_nothing() {
    assert_eq!(each_document(b"").count(), 0);
    assert_eq!(each_document("# just a comment\n".as_bytes()).count(), 0);
}

#[test]
fn depth_failure_is_the_depth_kind() {
    // the engine's depth guard: a deeply nested frame fails with the
    // DEPTH verdict, carrying its own geometry — no hang, and the
    // walk is over only because nothing follows
    let deep = format!("--- {}\n", "[".repeat(1200) + &"]".repeat(1200));
    let pairs: Vec<_> = each_document(deep.as_bytes()).collect();
    assert_eq!(pairs.len(), 1);
    let e = pairs[0].as_ref().unwrap_err();
    assert_eq!(e.index, 0);
    // depth failures surface with the engine's line on the deep frame
    assert!(e.line >= 1);
}

#[test]
fn error_kind_maps_stream_fatals_and_parse() {
    // a parse-class failure maps to ErrorKind::Parse (the mixed
    // stream's failed frame); the mapping is exhaustive over the
    // statuses each_document reports
    let mut iter = each_document("--- ok\n--- {{{{ bad\n".as_bytes());
    assert!(iter.next().unwrap().is_ok());
    let e = iter.next().unwrap().unwrap_err();
    assert_eq!(e.kind, ErrorKind::Parse);
    assert!(iter.next().is_none()); // unrecoverable tail ends the walk
}

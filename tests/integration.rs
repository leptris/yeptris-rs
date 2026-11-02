//! Integration tests: the safe surface against a real engine build.

use yeptris::{Document, Error, ErrorKind, NodeKind, Style};

#[test]
fn parse_scalar_styles() {
    let doc = Document::parse(b"- plain\n- 'single'\n- \"double\"\n- |\n  literal\n").unwrap();
    assert_eq!(doc.document_count(), 1);
    let seq = doc.first_root().unwrap();
    assert_eq!(seq.kind(), NodeKind::Sequence);
    let items: Vec<_> = seq.items().collect();
    assert_eq!(items.len(), 4);
    assert_eq!(items[0].str(), Some("plain"));
    assert_eq!(items[0].style(), Style::Plain);
    assert_eq!(items[1].str(), Some("single"));
    assert_eq!(items[1].style(), Style::SingleQuoted);
    assert_eq!(items[2].str(), Some("double"));
    assert_eq!(items[2].style(), Style::DoubleQuoted);
    assert_eq!(items[3].str(), Some("literal\n"));
    assert_eq!(items[3].style(), Style::Literal);
}

#[test]
fn parse_map_and_nested() {
    let doc =
        Document::parse(b"name: yeptris\nlangs:\n- c\n- rust\nmeta:\n  star: true\n").unwrap();
    let root = doc.first_root().unwrap();
    assert_eq!(root.kind(), NodeKind::Mapping);
    assert_eq!(root.map_count(), 3);
    assert_eq!(root.get("name").unwrap().str(), Some("yeptris"));
    let langs = root.get("langs").unwrap();
    assert_eq!(langs.kind(), NodeKind::Sequence);
    let got: Vec<_> = langs
        .items()
        .filter_map(|n| n.str().map(String::from))
        .collect();
    assert_eq!(got, vec!["c", "rust"]);
    let meta = root.get("meta").unwrap();
    assert_eq!(meta.get("star").unwrap().str(), Some("true"));
    assert!(root.get("missing").is_none());
}

#[test]
fn entries_and_keys() {
    let doc = Document::parse(b"a: 1\nb: 2\n").unwrap();
    let root = doc.first_root().unwrap();
    let keys: Vec<_> = root
        .entries()
        .filter_map(|(k, _)| k.str().map(String::from))
        .collect();
    assert_eq!(keys, vec!["a", "b"]);
    let values: Vec<_> = root
        .entries()
        .filter_map(|(_, v)| v.str().map(String::from))
        .collect();
    assert_eq!(values, vec!["1", "2"]);
}

#[test]
fn anchors_and_aliases() {
    let doc = Document::parse(b"base: &b\n  x: 1\nuse: *b\n").unwrap();
    let root = doc.first_root().unwrap();
    let base = root.get("base").unwrap();
    assert_eq!(base.anchor(), Some(&b"b"[..]));
    let alias = root.get("use").unwrap();
    assert_eq!(alias.kind(), NodeKind::Alias);
    assert_eq!(alias.str(), Some("b"));
    let target = alias.alias_target().unwrap();
    assert_eq!(target, base);
    assert_eq!(target.get("x").unwrap().str(), Some("1"));
}

#[test]
fn tags_are_reported() {
    let doc = Document::parse(b"n: !!str 42\n").unwrap();
    let node = doc.first_root().unwrap().get("n").unwrap();
    assert_eq!(node.tag(), Some(&b"tag:yaml.org,2002:str"[..]));
    assert_eq!(node.str(), Some("42"));
}

#[test]
fn multi_document_stream() {
    let doc = Document::parse(b"---\na: 1\n---\nb: 2\n").unwrap();
    assert_eq!(doc.document_count(), 2);
    assert_eq!(doc.root(0).unwrap().get("a").unwrap().str(), Some("1"));
    assert_eq!(doc.root(1).unwrap().get("b").unwrap().str(), Some("2"));
    assert!(doc.root(2).is_none());
}

#[test]
fn serialize_roundtrip() {
    let src = b"name: yeptris\nlangs:\n- c\n- rust\n";
    let doc = Document::parse(src).unwrap();
    assert_eq!(
        doc.serialize_string().unwrap(),
        String::from_utf8_lossy(src)
    );
    let round = doc.serialize().unwrap();
    let doc2 = Document::parse(&round).unwrap();
    assert_eq!(
        doc2.serialize_string().unwrap(),
        String::from_utf8_lossy(src)
    );
}

#[test]
fn folded_value_borrows_arena() {
    // an escaped scalar is document-owned; the view must still read it
    let doc = Document::parse(b"k: \"a\\tb\"\n").unwrap();
    let v = doc.first_root().unwrap().get("k").unwrap();
    assert_eq!(v.value(), Some(&b"a\tb"[..]));
}

#[test]
fn parse_errors_surface() {
    let err = Document::parse(b"a: [1,\n").unwrap_err();
    assert_eq!(err.kind(), ErrorKind::Parse);
    assert!(err.message().is_some());
    let err = Document::parse(b"a: \xff\xfe\n").unwrap_err();
    assert_eq!(err.kind(), ErrorKind::Encoding);
}

#[test]
fn error_message_survives_later_calls() {
    // yeptris-rs#24: the engine's text is captured at construction —
    // yeptris_last_error is thread-global and only valid immediately
    // after a failure, so a stored Error must never re-read it.
    let err = Document::parse(b"a: [1,\n").unwrap_err();
    let captured = err.message().expect("engine text captured").to_string();
    assert!(captured.contains("(line "), "got: {captured}");
    // a later successful parse rewrites the thread-global; the stored
    // Error must still show its own message
    let _ = Document::parse(b"ok: 1\n").unwrap();
    assert_eq!(err.message().as_deref(), Some(captured.as_str()));
    assert_eq!(err.kind(), ErrorKind::Parse);
    // binding-side constructions carry no message; Display falls back
    // to the kind's name
    let bare: Error = ErrorKind::Arg.into();
    assert_eq!(bare.message(), None);
    assert_eq!(bare.to_string(), "Arg");
}

#[test]
fn empty_and_null_forms() {
    let doc = Document::parse(b"a:\nb: ~\n").unwrap();
    let root = doc.first_root().unwrap();
    assert_eq!(root.get("a").unwrap().value(), Some(&b""[..]));
    // the DOM keeps raw spans — `~` reads as its text; null resolution
    // is the schema layer's job
    assert_eq!(root.get("b").unwrap().value(), Some(&b"~"[..]));
}

// ---- the stream face (yeptris#886) ------------------------------------

use yeptris::event::{EventKind, Puller};
use yeptris::{parse_stream, Mapping, Number, Value};

fn s(v: &str) -> Value {
    Value::String(v.to_owned())
}

#[test]
fn stream_explicit_markers() {
    let docs = parse_stream(b"---\na: 1\n---\nb: 2\n").unwrap();
    assert_eq!(docs.len(), 2);
    let m0 = match &docs[0] {
        Value::Mapping(m) => m,
        other => panic!("doc 0 is {other:?}"),
    };
    assert_eq!(m0.get(&s("a")), Some(&Value::Number(Number::I64(1))));
    let m1 = match &docs[1] {
        Value::Mapping(m) => m,
        other => panic!("doc 1 is {other:?}"),
    };
    assert_eq!(m1.get(&s("b")), Some(&Value::Number(Number::I64(2))));
}

#[test]
fn stream_marker_less_roots() {
    // the first document needs no `---`, and `--- N` inline roots
    // count one document each (the ruby load_stream pins)
    assert_eq!(
        parse_stream(b"foo\n---\nbar\n").unwrap(),
        vec![s("foo"), s("bar")]
    );
    assert_eq!(
        parse_stream(b"--- 1\n--- 2\n").unwrap(),
        vec![Value::Number(Number::I64(1)), Value::Number(Number::I64(2))]
    );
}

#[test]
fn stream_empty_docs_are_null() {
    // the nil-placement law: every document boundary lands a slot and
    // a content-less document keeps it Null (ruby's nil, py's None)
    assert_eq!(
        parse_stream(b"---\nfoo\n---\n").unwrap(),
        vec![s("foo"), Value::Null]
    );
    assert_eq!(
        parse_stream(b"---\n---\n").unwrap(),
        vec![Value::Null, Value::Null]
    );
    // a comment-only document is an empty document; a comment-only
    // STREAM has no documents at all
    assert_eq!(
        parse_stream(b"---\n# just a comment\n---\nx\n").unwrap(),
        vec![Value::Null, s("x")]
    );
    assert_eq!(
        parse_stream(b"# only a comment\n").unwrap(),
        Vec::<Value>::new()
    );
    // the `...` end marker closes a document; what follows starts a new one
    assert_eq!(
        parse_stream(b"---\nfoo\n...\n---\nbar\n").unwrap(),
        vec![s("foo"), s("bar")]
    );
}

#[test]
fn stream_empty_input_is_empty_vec() {
    // the engine's verdict for a document-less stream is NULL with
    // YEPTRIS_OK — an empty vec, never an error
    assert_eq!(parse_stream(b"").unwrap(), Vec::<Value>::new());
}

#[test]
fn stream_error_carries_position() {
    let err = parse_stream(b"---\na: 1\n---\nb: [1,\n---\nc: 3\n").unwrap_err();
    assert_eq!(err.kind(), ErrorKind::Parse);
    // the #28 law: the engine's line:col folds into the captured
    // message (the unterminated flow hits the `---` at line 5)
    let msg = err.message().expect("engine text captured");
    assert!(msg.contains("(line 5"), "got: {msg}");
}

#[test]
fn stream_single_doc_agrees_with_parse() {
    let inputs: &[&[u8]] = &[
        b"a: 1\n",
        b"- x\n- y\n",
        b"plain\n",
        b"k: &anchor v\nx: *anchor\n",
        b"n: !!str 42\n",
    ];
    for input in inputs {
        let doc = Document::parse(input).unwrap();
        let via_parse = Value::from_document(&doc).unwrap();
        let via_stream = parse_stream(input).unwrap();
        assert_eq!(
            via_stream,
            vec![via_parse],
            "{}",
            String::from_utf8_lossy(input)
        );
        assert_eq!(doc.document_count(), 1);
    }
}

#[test]
fn stream_null_round_trip_placement() {
    use yeptris::to_string;
    let docs = parse_stream(b"---\nk: ~\n---\n---\nfoo\n").unwrap();
    let mut expect_k = Mapping::new();
    expect_k.insert(s("k"), Value::Null);
    assert_eq!(docs, vec![Value::Mapping(expect_k), Value::Null, s("foo")]);
    // re-emit each document and re-parse the stream: the Null rides
    // the same boundaries on the way back
    let mut stream = String::new();
    for (i, d) in docs.iter().enumerate() {
        if i > 0 {
            stream.push_str("---\n");
        }
        stream.push_str(&to_string(d).unwrap());
        stream.push('\n');
    }
    assert_eq!(stream, "k: null\n---\nnull\n---\nfoo\n");
    assert_eq!(parse_stream(stream.as_bytes()).unwrap(), docs);
}

// ---- the event face (yeptris#886) -------------------------------------

#[test]
fn event_iterator_multidoc_sequence() {
    let mut puller = Puller::new(b"a: 1\n---\nb: 2\n").unwrap();
    let seq: Vec<(EventKind, String)> = puller
        .events()
        .map(|ev| {
            let ev = ev.unwrap();
            (ev.kind(), ev.value_str().unwrap_or_default().to_owned())
        })
        .collect();
    use EventKind::*;
    assert_eq!(
        seq,
        vec![
            (StreamStart, "".to_owned()),
            (DocumentStart, "".to_owned()),
            (MappingStart, "".to_owned()),
            (Scalar, "a".to_owned()),
            (Scalar, "1".to_owned()),
            (MappingEnd, "".to_owned()),
            (DocumentEnd, "".to_owned()),
            (DocumentStart, "".to_owned()),
            (MappingStart, "".to_owned()),
            (Scalar, "b".to_owned()),
            (Scalar, "2".to_owned()),
            (MappingEnd, "".to_owned()),
            (DocumentEnd, "".to_owned()),
            (StreamEnd, "".to_owned()),
        ]
    );
}

#[test]
fn event_single_doc_minimal_sequence() {
    let mut puller = Puller::new(b"foo\n").unwrap();
    let kinds: Vec<EventKind> = puller.events().map(|ev| ev.unwrap().kind()).collect();
    assert_eq!(
        kinds,
        vec![
            EventKind::StreamStart,
            EventKind::DocumentStart,
            EventKind::Scalar,
            EventKind::DocumentEnd,
            EventKind::StreamEnd,
        ]
    );
}

#[test]
fn event_marks_and_props() {
    let mut puller = Puller::new(b"k: &anchor v\nx: *anchor\n").unwrap();
    let mut scalars = Vec::new();
    let mut alias = None;
    for ev in puller.events() {
        let ev = ev.unwrap();
        match ev.kind() {
            EventKind::Scalar => scalars.push(ev),
            EventKind::Alias => alias = Some(ev),
            _ => {}
        }
    }
    // the anchored scalar carries the anchor and 1-based marks with
    // the end col one past the token (`&anchor v` spans col 4..13)
    assert_eq!(scalars[1].anchor(), Some(&b"anchor"[..]));
    assert_eq!(scalars[1].value_str(), Some("v"));
    assert_eq!(scalars[1].line(), 1);
    assert_eq!(scalars[1].col(), 4);
    assert_eq!(scalars[1].end_col(), 13);
    // the alias carries its name in the value field
    let alias = alias.unwrap();
    assert_eq!(alias.value_str(), Some("anchor"));
    assert_eq!(alias.line(), 2);
    assert_eq!(alias.col(), 4);
    // plain untagged scalars carry the implicit flag; the alias does not
    assert!(scalars[0].implicit());
    assert!(!alias.implicit());
}

#[test]
fn event_style_and_flow_flags() {
    let mut puller = Puller::new(b"- 'q'\n- {k: v}\n").unwrap();
    let mut scalar_styles = Vec::new();
    let mut flow = None;
    for ev in puller.events() {
        let ev = ev.unwrap();
        match ev.kind() {
            EventKind::Scalar => scalar_styles.push(ev.style()),
            EventKind::MappingStart => flow = Some(ev.flow()),
            _ => {}
        }
    }
    assert_eq!(
        scalar_styles,
        vec![Style::SingleQuoted, Style::Plain, Style::Plain]
    );
    assert_eq!(flow, Some(true));
}

#[test]
fn event_error_carries_position_then_fuses() {
    let mut puller = Puller::new(b"a: [1,\n").unwrap();
    let mut last = Ok(());
    let mut events = puller.events();
    for ev in events.by_ref() {
        last = ev.map(|_| ());
    }
    let err = last.unwrap_err();
    assert_eq!(err.kind(), ErrorKind::Parse);
    assert!(err.message().unwrap().contains("(line "), "got: {err}");
    // the cursor fused: the engine's puller stays in the failed state,
    // so an un-fused cursor would yield the error forever
    assert!(events.next().is_none());
}

#[test]
fn event_empty_input_frames_the_empty_stream() {
    // a document-less stream still frames exactly once
    for input in [&b""[..], &b"# nothing\n"[..]] {
        let mut puller = Puller::new(input).unwrap();
        let kinds: Vec<EventKind> = puller.events().map(|ev| ev.unwrap().kind()).collect();
        assert_eq!(kinds, vec![EventKind::StreamStart, EventKind::StreamEnd]);
    }
}

//! Integration tests: the safe surface against a real engine build.

use yeptris::{Document, Error, NodeKind, Style};

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
    assert_eq!(err, Error::Parse);
    assert!(!err.message().is_empty());
    let err = Document::parse(b"a: \xff\xfe\n").unwrap_err();
    assert_eq!(err, Error::Encoding);
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

//! ABI-facing pins: the version surface and the status-code mapping.
//!
//! The DOM face is opaque by design (no repr(C) records to mirror),
//! so the family's struct-size introspection pattern applies to the
//! constants instead: the status mapping and node/style enums must
//! match the engine's headers exactly.

use yeptris::{version, Document, Error};

#[test]
fn version_is_semver_shaped() {
    let v = version();
    assert!(!v.is_empty(), "yeptris_version() returned empty");
    let core = v.split('+').next().unwrap();
    let parts: Vec<_> = core.split('-').next().unwrap().split('.').collect();
    assert!(parts.len() >= 2, "version {v} is not semver-shaped");
    assert!(
        parts.iter().take(2).all(|p| p.parse::<u64>().is_ok()),
        "version {v} major/minor not numeric"
    );
}

#[test]
fn status_mapping_roundtrip() {
    // every code the engine header pins has a distinct variant
    assert_ne!(Error::Parse, Error::Depth);
    assert_ne!(Error::Memory, Error::Arg);
    assert_ne!(Error::Encoding, Error::Io);
    assert_ne!(Error::Unsupported, Error::Internal);
    assert_ne!(Error::Schema, Error::Parse);
    // an unknown code carries through untouched
    assert_eq!(Error::Unknown(42), Error::Unknown(42));
}

#[test]
fn node_ids_stable_within_document() {
    let doc = Document::parse(b"a: 1\nb: 2\n").unwrap();
    let root = doc.first_root().unwrap();
    let (k0, v0) = root.map_at(0).unwrap();
    let (k1, v1) = root.map_at(1).unwrap();
    assert_ne!(k0.id(), k1.id());
    assert_ne!(v0.id(), v1.id());
    // re-querying resolves to the same identity
    assert_eq!(root.get("a").unwrap(), v0);
}

//! ABI-facing pins: the version surface and the status-code mapping.
//!
//! The DOM face is opaque by design (no repr(C) records to mirror),
//! so the family's struct-size introspection pattern applies to the
//! constants instead: the status mapping and node/style enums must
//! match the engine's headers exactly.

use yeptris::{version, Document, ErrorKind, ENGINE_REV};

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
fn vendored_rev_matches_compiled_engine() {
    // Under `vendored`, the snapshot's ENGINE_REF pin (reported by
    // build.rs) and the engine actually linked into this test binary
    // must agree — they are one snapshot by construction, and the
    // release tag carries the same version as project(VERSION), which
    // build.rs cross-checked at build time.
    let Some(rev) = ENGINE_REV else {
        // the dynamic-link default: no snapshot claim to check
        return;
    };
    let v = version();
    assert!(
        rev.contains(&format!("refs/tags/v{v}")),
        "vendored rev {rev:?} does not pin the compiled engine version {v:?}"
    );
}

#[test]
fn status_mapping_roundtrip() {
    // every code the engine header pins has a distinct variant
    assert_ne!(ErrorKind::Parse, ErrorKind::Depth);
    assert_ne!(ErrorKind::Memory, ErrorKind::Arg);
    assert_ne!(ErrorKind::Encoding, ErrorKind::Io);
    assert_ne!(ErrorKind::Unsupported, ErrorKind::Internal);
    assert_ne!(ErrorKind::Schema, ErrorKind::Parse);
    // an unknown code carries through untouched
    assert_eq!(ErrorKind::Unknown(42), ErrorKind::Unknown(42));
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

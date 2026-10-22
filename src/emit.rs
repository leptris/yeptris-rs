//! `to_string` — the dump lane: a [`Value`] packs into the engine's
//! bulk build entries (one FFI call) and the engine's emitter writes
//! the bytes. A string rides PLAIN exactly when it would re-parse as
//! the same string under the core-12 schema (the resolver IS the
//! gate, the py binding's law); everything else rides double-quoted.

use crate::ffi::{self, YeptrisBuildEntry};
use crate::value::{resolve_plain, Number, Value};
use crate::{Document, Error, Result};

/// Serialize a [`Value`] to YAML (no trailing newline — serde_yaml
/// parity). Rides the engine emitter via the bulk build API.
pub fn to_string(value: &Value) -> Result<String> {
    let mut b = Builder::default();
    b.push(value)?;
    let doc = Document::new()?;
    let status = unsafe {
        ffi::yeptris_document_build(
            doc.raw(),
            b.entries.as_ptr(),
            b.entries.len(),
            b.blob.as_ptr() as *const std::os::raw::c_char,
            b.blob.len(),
        )
    };
    if status != ffi::YEPTRIS_OK {
        return Err(Error::from_status(status));
    }
    let s = doc.serialize_string()?;
    Ok(s.strip_suffix('\n').unwrap_or(&s).to_owned())
}

#[derive(Default)]
struct Builder {
    entries: Vec<YeptrisBuildEntry>,
    blob: Vec<u8>,
}

impl Builder {
    fn place(&mut self, op: u8, style: u8, bytes: &[u8]) -> Result<()> {
        let off = u32::try_from(self.blob.len()).map_err(|_| Error::Arg)?;
        let len = u32::try_from(bytes.len()).map_err(|_| Error::Arg)?;
        self.blob.extend_from_slice(bytes);
        self.entries.push(YeptrisBuildEntry {
            op,
            style,
            reserved: 0,
            off,
            len,
        });
        Ok(())
    }

    fn push(&mut self, v: &Value) -> Result<()> {
        match v {
            Value::Null => self.place(
                ffi::YEPTRIS_BUILD_SCALAR,
                ffi::YEPTRIS_STYLE_PLAIN as u8,
                b"null",
            ),
            Value::Bool(b) => self.place(
                ffi::YEPTRIS_BUILD_SCALAR,
                ffi::YEPTRIS_STYLE_PLAIN as u8,
                if *b { b"true" } else { b"false" },
            ),
            Value::Number(n) => {
                let text = match n {
                    Number::I64(x) => x.to_string(),
                    Number::U64(x) => x.to_string(),
                    Number::F64(x) => x.to_string(),
                };
                self.place(
                    ffi::YEPTRIS_BUILD_SCALAR,
                    ffi::YEPTRIS_STYLE_PLAIN as u8,
                    text.as_bytes(),
                )
            }
            Value::String(s) => {
                if plain_safe(s) {
                    self.place(
                        ffi::YEPTRIS_BUILD_SCALAR,
                        ffi::YEPTRIS_STYLE_PLAIN as u8,
                        s.as_bytes(),
                    )
                } else {
                    self.place(
                        ffi::YEPTRIS_BUILD_SCALAR,
                        ffi::YEPTRIS_STYLE_DOUBLE_QUOTED as u8,
                        s.as_bytes(),
                    )
                }
            }
            Value::Sequence(items) => {
                self.entries.push(entry(ffi::YEPTRIS_BUILD_SEQ));
                for item in items {
                    self.push(item)?;
                }
                self.entries.push(entry(ffi::YEPTRIS_BUILD_END));
                Ok(())
            }
            Value::Mapping(map) => {
                self.entries.push(entry(ffi::YEPTRIS_BUILD_MAP));
                for (k, v) in &map.0 {
                    self.push(k)?;
                    self.push(v)?;
                }
                self.entries.push(entry(ffi::YEPTRIS_BUILD_END));
                Ok(())
            }
            Value::Tagged(t) => {
                self.push(&t.value)?;
                self.place(ffi::YEPTRIS_BUILD_TAG, 0, t.tag.as_bytes())
            }
        }
    }
}

fn entry(op: u8) -> YeptrisBuildEntry {
    YeptrisBuildEntry {
        op,
        style: 0,
        reserved: 0,
        off: 0,
        len: 0,
    }
}

/// A string dumps PLAIN exactly when it re-parses as the same string
/// under the loading schema AND carries no leading indicator or
/// embedded structure byte the scanner would tokenize.
pub(crate) fn plain_safe(s: &str) -> bool {
    if s.is_empty()
        || s.starts_with(' ')
        || s.ends_with(' ')
        || s.contains('\n')
        || s.contains('\t')
        || s.contains(": ")
        || s.contains(" #")
        || s.ends_with(':')
    {
        return false;
    }
    let first = s.as_bytes()[0];
    const FLOW: &[u8] = b",[]{}#&*!|>'\"%@`";
    if FLOW.contains(&first) {
        return false;
    }
    // "-?:": plain-safe when glued to non-space content ("-foo"), the
    // indicator only tokenizes before a blank
    if (first == b'-' || first == b'?' || first == b':') && s.len() > 1 && s.as_bytes()[1] == b' ' {
        return false;
    }
    // the schema gate: a resolvable shape (null/bool/int/float) must
    // not ride plain — it would re-parse as another type
    resolve_plain(s.as_bytes()) == Value::String(s.to_owned())
}

//! `to_string` — the dump lane: a [`Value`] packs into the engine's
//! bulk build entries (one FFI call) and the engine's emitter writes
//! the bytes. A string rides PLAIN exactly when it would re-parse as
//! the same string under the core-12 schema (the resolver IS the
//! gate, the py binding's law); everything else rides double-quoted.

use crate::ffi::{self, YeptrisBuildEntry};
use crate::value::{resolve_plain, Number, Value};
use crate::{Document, Error, ErrorKind, Result};

/// Pack a [`Value`] into a fresh document through the bulk build API
/// (the format-agnostic first half of the dump lane: the JSON and
/// CBOR faces serialize the same build).
pub(crate) fn build_document(value: &Value) -> Result<Document<'static>> {
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
    Ok(doc)
}

/// Serialize a [`Value`] to YAML (no trailing newline — serde_yaml
/// parity). Rides the engine emitter via the bulk build API.
pub fn to_string(value: &Value) -> Result<String> {
    let doc = build_document(value)?;
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
        let off = u32::try_from(self.blob.len()).map_err(|_| ErrorKind::Arg)?;
        let len = u32::try_from(bytes.len()).map_err(|_| ErrorKind::Arg)?;
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
/// embedded structure byte the scanner would tokenize. The refusal set
/// mirrors the engine's plain-safety truth table (scan/scan.c's
/// `yep_scan_plain_safe` plus the emitter's control-byte rule) so the
/// dump lane cannot emit a plain form the engine itself would refuse
/// to re-parse.
pub(crate) fn plain_safe(s: &str) -> bool {
    let b = s.as_bytes();
    if b.is_empty()
        || is_blank(b[0])
        || is_blank(b[b.len() - 1])
        // a LEADING U+FEFF re-parses as a document BOM and vanishes
        // (scan.c: the emitted form must carry its escape); a
        // mid-token BOM is content
        || b.starts_with(b"\xef\xbb\xbf")
    {
        return false;
    }
    // the indicator head: none of these may open a plain scalar
    const FLOW: &[u8] = b",[]{}#&*!|>'\"%@`";
    if FLOW.contains(&b[0]) {
        return false;
    }
    // "-?:": the BARE one-character forms are the indicators
    // themselves ("-" re-reads as a sequence entry, "?" as an
    // explicit key) — glued forms ("-foo") are safe unless a BLANK
    // (space OR tab) follows the indicator: "-\tx" re-reads as a
    // sequence entry carrying "x"
    if (b[0] == b'-' || b[0] == b'?' || b[0] == b':') && (b.len() == 1 || is_blank(b[1])) {
        return false;
    }
    // the document markers: a plain "---"/"..." (or either as a
    // prefix before a blank) re-reads as a document boundary at line
    // start ("--- x" parses as the string "x", "... x" is a parse
    // error); the glued "---x" is content
    if (b.starts_with(b"---") || b.starts_with(b"..."))
        && (b.len() == 3 || is_blank(b[3]) || b[3] == b'\n' || b[3] == b'\r')
    {
        return false;
    }
    // the interior walk. Controls first: C0 (0x00..0x1F — the breaks
    // LF/CR and TAB included) and DEL have no plain spelling the
    // scanner accepts (it answers ENCODING); the C1 block
    // U+0080..U+009F (raw NEL U+0085 re-reads as a break) and the
    // plane-edge noncharacters U+FFFE/U+FFFF are refused by the same
    // printability gate; raw LS/PS re-read as line breaks. Only the
    // double-quoted escape lane survives any of them.
    let mut i = 0;
    while i < b.len() {
        let c = b[i];
        if c < 0x20 || c == 0x7f {
            return false;
        }
        if c == 0xc2 && i + 1 < b.len() && (0x80..=0x9f).contains(&b[i + 1]) {
            return false;
        }
        if c == 0xe2 && i + 2 < b.len() && b[i + 1] == 0x80 && (b[i + 2] & 0xfe) == 0xa8 {
            return false;
        }
        if c == 0xef && i + 2 < b.len() && b[i + 1] == 0xbf && (b[i + 2] & 0xfe) == 0xbe {
            return false;
        }
        // ':' ends a plain only before a BLANK or at the span's end
        // ("a:\tb" re-reads as a mapping entry); '#' starts a comment
        // after a BLANK ("a\t#b" re-reads as "a" + a comment). A
        // leading '#' never gets here (the FLOW head already refused).
        if c == b':' && (i + 1 == b.len() || is_blank(b[i + 1])) {
            return false;
        }
        if c == b'#' && is_blank(b[i - 1]) {
            return false;
        }
        i += 1;
    }
    // the schema gate: a resolvable shape (null/bool/int/float) must
    // not ride plain — it would re-parse as another type
    resolve_plain(b) == Value::String(s.to_owned())
}

/// The engine's blank class: space or TAB (a tab after "-"/":" or
/// before "#" is structure exactly like a space).
fn is_blank(c: u8) -> bool {
    c == b' ' || c == b'\t'
}

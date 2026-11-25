//! The engine's JSON face (yeptris-rs#17): the same document model the
//! YAML face wraps. Parsing rides the engine's compact JSON tape
//! (yeptris/tape.h) — one engine pass records the token columns and
//! one bulk call converts the number spans, then a tight host walk
//! builds the [`Value`] tree: two FFI crossings total, where the old
//! DOM route paid a handful of round-trips per node (yeptris-rs#104 —
//! the binding layer was ~97% of `json::parse`'s wall time).
//! Serialization rides a direct host writer that emits the engine's
//! canonical JSON byte-for-byte (differentially pinned), with the
//! engine route kept for the corners JSON cannot spell (tagged
//! values) — see [`serialize`].

use std::ffi::c_char;
use std::io::Write as _;

use serde::de::DeserializeOwned;
use serde::Serialize;

use crate::value::{Mapping, Number, Value};
use crate::{Document, Error, ErrorKind, Result};

use crate::emit;
use crate::ffi;

impl Document<'_> {
    /// Parse JSON from bytes. The engine borrows the buffer — hold it
    /// while the [`Document`] lives.
    pub fn parse_json<'a>(input: &'a [u8]) -> Result<Document<'a>> {
        let mut status: ffi::YeptrisStatus = ffi::YEPTRIS_OK;
        let raw = unsafe {
            ffi::yeptris_parse_json(input.as_ptr() as *const c_char, input.len(), &mut status)
        };
        if raw.is_null() {
            return Err(Error::from_status(status));
        }
        Ok(Document {
            raw,
            _input: core::marker::PhantomData,
        })
    }

    /// Serialize the whole stream to canonical JSON bytes.
    ///
    /// Memory: the engine's buffer is copied before the free; the
    /// returned `Vec` owns its bytes.
    ///
    /// NULL here is json.h's no-spelling verdict (a document with an
    /// unspellable flow anchor has no JSON representation) — not an
    /// allocation failure, so it maps to
    /// [`ErrorKind::NotRepresentable`], never [`ErrorKind::Memory`].
    pub fn serialize_json(&self) -> Result<Vec<u8>> {
        let mut len: usize = 0;
        let raw = unsafe { ffi::yeptris_serialize_json(self.raw, &mut len) };
        if raw.is_null() {
            return Err(Error::not_representable("JSON"));
        }
        let out = unsafe { std::slice::from_raw_parts(raw as *const u8, len) }.to_vec();
        unsafe { ffi::yeptris_free(raw as *mut core::ffi::c_void) };
        Ok(out)
    }

    /// Serialize to canonical JSON as an owned UTF-8 string.
    pub fn serialize_json_string(&self) -> Result<String> {
        let bytes = self.serialize_json()?;
        String::from_utf8(bytes).map_err(|_| Error::from(ErrorKind::Internal))
    }
}

/// Parse JSON text into a [`Value`].
///
/// The tape route: one strict engine walk records the token tape, one
/// bulk call materializes the numbers, and the host builds the tree
/// from the columns. Every tape-route failure defers to the DOM face
/// ([`Document::parse_json`]), which owns the exact acceptance and
/// error contracts — including the >1024-character simple-key law the
/// tape walk itself does not enforce (yeptris-rs#104).
pub fn parse(s: &str) -> Result<Value> {
    parse_bytes(s.as_bytes())
}

/// The byte-level entry ([`from_slice`]): the engine validates the
/// UTF-8 either way, so bytes pass through untouched.
fn parse_bytes(src: &[u8]) -> Result<Value> {
    match parse_tape(src) {
        Ok(v) => Ok(v),
        // The DOM route decides: its Value (when the tape route hit
        // the key law or a shape it does not carry) or its verbatim
        // error.
        Err(_) => {
            let doc = Document::parse_json(src)?;
            Value::from_document(&doc)
        }
    }
}

/// One strict tape parse plus the bulk host build. `Err` here is the
/// FALLBACK signal — never surfaced directly ([`parse`]).
fn parse_tape(src: &[u8]) -> Result<Value> {
    let mut tape = ffi::YeptrisJsonTape::ZEROED;
    let st = unsafe {
        ffi::yeptris_parse_json_tape(src.as_ptr() as *const c_char, src.len(), &mut tape)
    };
    if st != ffi::YEPTRIS_OK {
        return Err(Error::from_status(st));
    }
    let out = build_value(src, &tape);
    unsafe { ffi::yeptris_tape_free(&mut tape) };
    out
}

/// One builder frame. The pending key rides the map frame (a string
/// record lands in it; the next value closes the pair).
enum Frame {
    Seq(Vec<Value>),
    Map(Mapping, Option<Value>),
}

/// The bulk tape → Value builder (the ruby binding's #81 walk, host
/// edition): reads the three columns as slices, materializes every
/// number span in ONE engine call, and places records with an
/// explicit frame stack — no recursion, no per-node FFI.
fn build_value(src: &[u8], t: &ffi::YeptrisJsonTape) -> Result<Value> {
    debug_assert_eq!(t.rec_primary, 0); // the strict route is column-primary
    let n = t.count;
    let kinds = unsafe { std::slice::from_raw_parts(t.kinds, n) };
    let offs = unsafe { std::slice::from_raw_parts(t.offs, n) };
    let lens = unsafe { std::slice::from_raw_parts(t.lens, n) };
    // the C writes are record-indexed (tape.h): buffers span [0, n)
    let mut ivals = vec![0i64; n];
    let mut dvals = vec![0.0f64; n];
    let converted = unsafe {
        ffi::yeptris_tape_convert(
            t as *const ffi::YeptrisJsonTape as *mut ffi::YeptrisJsonTape,
            0,
            n,
            ivals.as_mut_ptr(),
            dvals.as_mut_ptr(),
        )
    };
    if converted == usize::MAX {
        // impossible for a tape the strict route produced; the DOM
        // fallback re-decides the input
        return Err(ErrorKind::Internal.into());
    }
    // int_min is set by the convert call when an INT span exceeds
    // int64: every INT then rebuilds from its text (the DOM route's
    // typing: i64, u64's positive window, else the literal string)
    let int_exact = t.int_min == 0;

    let mut root: Option<Value> = None;
    let mut stack: Vec<Frame> = Vec::new();
    // record 0 is the DOC boundary; the records start at 1
    for i in 1..n {
        match kinds[i] {
            ffi::YEP_T_SEQ_OPEN => {
                // the matching CLOSE links from the off column; the
                // record delta bounds the item count (upper), the hint
                // stays small — amortized growth is cheaper than the
                // over-reservation a nested-heavy document would pay
                let hint = ((offs[i] as usize) - i).min(16);
                stack.push(Frame::Seq(Vec::with_capacity(hint)));
            }
            ffi::YEP_T_MAP_OPEN => stack.push(Frame::Map(Mapping::new(), None)),
            ffi::YEP_T_CLOSE => {
                let frame = stack.pop().ok_or(ErrorKind::Internal)?;
                let v = match frame {
                    Frame::Seq(items) => Value::Sequence(items),
                    // a pending key without its value cannot survive a
                    // strict tape (the walk's key/value alternation)
                    Frame::Map(map, pending) => {
                        debug_assert!(pending.is_none());
                        Value::Mapping(map)
                    }
                };
                place(&mut root, &mut stack, v)?;
            }
            ffi::YEP_T_STR => {
                let (off, len) = (offs[i] as usize, lens[i] as usize);
                let raw = &src[off..off + len];
                // the key slot: the top frame is a map without a
                // pending key (the walk's alternation guarantees it)
                if let Some(Frame::Map(_, slot @ None)) = stack.last_mut() {
                    // the simple-key law (libyaml's 1024-character
                    // budget): the tape walk does not enforce it, the
                    // DOM face does — a violation defers the whole
                    // value to the DOM route, which owns the error
                    if raw.len() > 1022 && raw.len() - cont_bytes(raw) + 2 > 1024 {
                        return Err(ErrorKind::Parse.into());
                    }
                    *slot = Some(Value::String(decode_json_str(raw)));
                    continue;
                }
                place(&mut root, &mut stack, Value::String(decode_json_str(raw)))?;
            }
            ffi::YEP_T_INT => {
                let v = if int_exact {
                    Value::Number(Number::I64(ivals[i]))
                } else {
                    let (off, len) = (offs[i] as usize, lens[i] as usize);
                    match int_span_value(&src[off..off + len]) {
                        Some(v) => v,
                        None => return Err(ErrorKind::Internal.into()),
                    }
                };
                place(&mut root, &mut stack, v)?;
            }
            ffi::YEP_T_FLOAT => place(&mut root, &mut stack, Value::Number(Number::F64(dvals[i])))?,
            ffi::YEP_T_TRUE => place(&mut root, &mut stack, Value::Bool(true))?,
            ffi::YEP_T_FALSE => place(&mut root, &mut stack, Value::Bool(false))?,
            ffi::YEP_T_NULL => place(&mut root, &mut stack, Value::Null)?,
            // NUM (lenient) and CONT (>16 MiB spans) never reach a
            // strict-route tape; the DOM fallback re-decides
            _ => return Err(ErrorKind::Internal.into()),
        }
    }
    if !stack.is_empty() {
        return Err(ErrorKind::Internal.into()); // unbalanced: impossible; defer
    }
    root.ok_or_else(|| ErrorKind::Internal.into())
}

/// Place one value into its parent (or the root slot).
fn place(root: &mut Option<Value>, stack: &mut [Frame], v: Value) -> Result<()> {
    match stack.last_mut() {
        None => {
            if root.replace(v).is_some() {
                return Err(ErrorKind::Internal.into()); // two roots: impossible
            }
        }
        Some(Frame::Seq(items)) => items.push(v),
        Some(Frame::Map(map, pending)) => match pending.take() {
            Some(k) => map.insert(k, v),
            None => *pending = Some(v),
        },
    }
    Ok(())
}

/// The strict tape's INT spans are `-?[0-9]+`: the DOM route's typing
/// is i64, then u64's positive window, else the literal text rides as
/// a string (`None` — invalid UTF-8 — cannot happen on a validated
/// span; it defers to the DOM fallback).
fn int_span_value(raw: &[u8]) -> Option<Value> {
    let text = std::str::from_utf8(raw).ok()?;
    if let Ok(v) = text.parse::<i64>() {
        return Some(Value::Number(Number::I64(v)));
    }
    if let Ok(v) = text.parse::<u64>() {
        return Some(Value::Number(Number::U64(v)));
    }
    Some(Value::String(text.to_owned()))
}

/// Continuation-byte count (bytes with `10xxxxxx`): the simple-key
/// budget counts characters, not bytes (libyaml's law).
fn cont_bytes(raw: &[u8]) -> usize {
    raw.iter().filter(|b| (*b & 0xC0) == 0x80).count()
}

fn hex4(raw: &[u8]) -> Option<u32> {
    if raw.len() < 4 {
        return None;
    }
    let mut cp: u32 = 0;
    for b in &raw[..4] {
        let d = match b {
            b'0'..=b'9' => u32::from(b - b'0'),
            b'a'..=b'f' => u32::from(b - b'a') + 10,
            b'A'..=b'F' => u32::from(b - b'A') + 10,
            _ => return None,
        };
        cp = (cp << 4) | d;
    }
    Some(cp)
}

/// The tape keeps RAW string spans (escapes untouched); decode the
/// JSON escape set into an owned `String`. The strict route has
/// already validated the grammar — only the eight simple escapes plus
/// `\uXXXX` with mandatory surrogate pairing — and the UTF-8, so the
/// no-escape fast path is a validate-and-copy.
fn decode_json_str(raw: &[u8]) -> String {
    if !raw.contains(&b'\\') {
        return match std::str::from_utf8(raw) {
            Ok(s) => s.to_owned(),
            // unreachable on a strict-route tape (the entry validated
            // the UTF-8); degrade the way the DOM route does
            Err(_) => String::from_utf8_lossy(raw).into_owned(),
        };
    }
    let mut out = String::with_capacity(raw.len());
    let mut i = 0;
    while i < raw.len() {
        if raw[i] == b'\\' {
            i += 1;
            match raw.get(i) {
                Some(b'"') => out.push('"'),
                Some(b'\\') => out.push('\\'),
                Some(b'/') => out.push('/'),
                Some(b'b') => out.push('\u{8}'),
                Some(b'f') => out.push('\u{c}'),
                Some(b'n') => out.push('\n'),
                Some(b'r') => out.push('\r'),
                Some(b't') => out.push('\t'),
                Some(b'u') => match hex4(&raw[i + 1..]) {
                    Some(cp) => {
                        i += 4;
                        let cp = if (0xD800..=0xDBFF).contains(&cp) {
                            // the scanner guarantees a paired low
                            // surrogate; read it, defensively degrade
                            // to U+FFFD exactly like the engine
                            match raw.get(i + 1..i + 4) {
                                Some([b'\\', b'u', ..]) => match hex4(&raw[i + 3..]) {
                                    Some(lo) if (0xDC00..=0xDFFF).contains(&lo) => {
                                        i += 6;
                                        0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00)
                                    }
                                    _ => 0xFFFD,
                                },
                                _ => 0xFFFD,
                            }
                        } else if (0xDC00..=0xDFFF).contains(&cp) {
                            0xFFFD // lone low: unreachable on strict input
                        } else {
                            cp
                        };
                        out.push(char::from_u32(cp).unwrap_or('\u{FFFD}'));
                    }
                    None => return String::from_utf8_lossy(raw).into_owned(),
                },
                // the strict grammar admits nothing else
                _ => return String::from_utf8_lossy(raw).into_owned(),
            }
            i += 1;
            continue;
        }
        // copy the run to the next escape (or the end)
        let start = i;
        while i < raw.len() && raw[i] != b'\\' {
            i += 1;
        }
        match std::str::from_utf8(&raw[start..i]) {
            Ok(s) => out.push_str(s),
            Err(_) => out.push_str(&String::from_utf8_lossy(&raw[start..i])),
        }
    }
    out
}

/// Serialize a [`Value`] to canonical JSON (compact, byte-stable).
///
/// The writer route (yeptris-rs#104): a direct host walk emits the
/// engine's canonical JSON shape — `": "` pair separators, `", "`
/// item separators, always-quoted strings, the json-gem float spell —
/// differentially pinned byte-for-byte against the engine route in
/// this module's tests. The engine route stays for the corners JSON
/// cannot spell: a [`Value::Tagged`] anywhere (the engine quotes a
/// tagged scalar's text and refuses a tagged container), and a
/// mapping key that is a sequence or mapping (the engine refuses
/// those too). Container trees past the engine's 1000-frame build cap
/// fail with [`ErrorKind::Depth`], exactly as the engine route does.
pub fn serialize(value: &Value) -> Result<String> {
    if has_tagged(value) {
        let doc = emit::build_document(value)?;
        return doc.serialize_json_string();
    }
    let mut out = Vec::with_capacity(256);
    write_value(&mut out, value, 0)?;
    // the engine's root law (writer.c): a non-scalar ROOT closes with
    // one newline; a scalar root rides bare
    if matches!(value, Value::Sequence(_) | Value::Mapping(_)) {
        out.push(b'\n');
    }
    String::from_utf8(out).map_err(|_| Error::from(ErrorKind::Internal))
}

/// Any [`Value::Tagged`] anywhere in the tree (iterative: the check
/// must survive host-built trees deeper than the writer's cap).
fn has_tagged(value: &Value) -> bool {
    let mut stack = vec![value];
    while let Some(v) = stack.pop() {
        match v {
            Value::Tagged(_) => return true,
            Value::Sequence(items) => stack.extend(items.iter()),
            Value::Mapping(map) => {
                for (k, v) in &map.0 {
                    stack.push(k);
                    stack.push(v);
                }
            }
            _ => {}
        }
    }
    false
}

/// The engine build's frame cap (dom.h YEP_DOM_MAX_DEPTH): entering
/// container level 1000 fails the build with DEPTH, message-less.
const WRITER_MAX_DEPTH: usize = 999;

fn write_value(out: &mut Vec<u8>, v: &Value, depth: usize) -> Result<()> {
    match v {
        Value::Null => out.extend_from_slice(b"null"),
        Value::Bool(true) => out.extend_from_slice(b"true"),
        Value::Bool(false) => out.extend_from_slice(b"false"),
        Value::Number(n) => write_number(out, *n),
        Value::String(s) => write_json_str(out, s),
        Value::Sequence(items) => {
            if depth >= WRITER_MAX_DEPTH {
                return Err(ErrorKind::Depth.into());
            }
            out.push(b'[');
            for (i, item) in items.iter().enumerate() {
                if i > 0 {
                    out.extend_from_slice(b", ");
                }
                write_value(out, item, depth + 1)?;
            }
            out.push(b']');
        }
        Value::Mapping(map) => {
            if depth >= WRITER_MAX_DEPTH {
                return Err(ErrorKind::Depth.into());
            }
            out.push(b'{');
            for (i, (k, val)) in map.0.iter().enumerate() {
                if i > 0 {
                    out.extend_from_slice(b", ");
                }
                match k {
                    Value::String(s) => write_json_str(out, s),
                    Value::Number(n) => write_number(out, *n),
                    Value::Null => out.extend_from_slice(b"null"),
                    Value::Bool(true) => out.extend_from_slice(b"true"),
                    Value::Bool(false) => out.extend_from_slice(b"false"),
                    // non-scalar keys have no JSON spelling (the
                    // engine build refuses them with a bare PARSE)
                    Value::Sequence(_) | Value::Mapping(_) => {
                        return Err(ErrorKind::Parse.into())
                    }
                    // unreachable: has_tagged routed the value away
                    Value::Tagged(_) => return Err(ErrorKind::Parse.into()),
                }
                out.extend_from_slice(b": ");
                write_value(out, val, depth + 1)?;
            }
            out.push(b'}');
        }
        // unreachable: has_tagged routed the value away
        Value::Tagged(_) => return Err(ErrorKind::Parse.into()),
    }
    Ok(())
}

fn write_number(out: &mut Vec<u8>, n: Number) {
    match n {
        Number::I64(v) => {
            let _ = write!(out, "{}", v);
        }
        Number::U64(v) => {
            let _ = write!(out, "{}", v);
        }
        Number::F64(x) => write_json_float(out, x),
    }
}

/// The engine's canonical JSON escaping (emit_dq_json): the named
/// controls, `\"`, `\\`, `\u00xx` for the other C0 bytes, raw UTF-8
/// through untouched.
fn write_json_str(out: &mut Vec<u8>, s: &str) {
    out.push(b'"');
    let b = s.as_bytes();
    let mut run = 0usize;
    for (i, &c) in b.iter().enumerate() {
        if c >= 0x20 && c != b'"' && c != b'\\' {
            run += 1;
            continue;
        }
        if run != 0 {
            out.extend_from_slice(&b[i - run..i]);
            run = 0;
        }
        match c {
            b'"' => out.extend_from_slice(b"\\\""),
            b'\\' => out.extend_from_slice(b"\\\\"),
            0x08 => out.extend_from_slice(b"\\b"),
            0x09 => out.extend_from_slice(b"\\t"),
            0x0a => out.extend_from_slice(b"\\n"),
            0x0c => out.extend_from_slice(b"\\f"),
            0x0d => out.extend_from_slice(b"\\r"),
            _ => {
                // the other C0 controls ride \u00xx
                const HEX: &[u8; 16] = b"0123456789abcdef";
                out.extend_from_slice(b"\\u00");
                out.push(HEX[usize::from(c >> 4)]);
                out.push(HEX[usize::from(c & 0xf)]);
            }
        }
    }
    if run != 0 {
        out.extend_from_slice(&b[b.len() - run..]);
    }
    out.push(b'"');
}

/// The engine's JSON float spell (`yep_d2s_json`, the json-gem fpconv
/// shape — yeptris#651), over Rust's shortest round-trip digits:
///
/// - non-finite values ride QUOTED as the core-schema words (no JSON
///   infinity), exactly as the emitter's float arm spells them;
/// - zero is `0.0` / `-0.0` (the sign survives);
/// - integral magnitudes under 1e15 are plain digits + `.0`;
/// - magnitudes down to 1e-7-scale plain decimals stay decimal;
/// - everything else is `d.ddde±XX`, two exponent digits minimum
///   (`1e-15`, `1.5e+15`, `1e+100`).
fn write_json_float(out: &mut Vec<u8>, x: f64) {
    if x.is_nan() {
        out.extend_from_slice(b"\".nan\"");
        return;
    }
    if x.is_infinite() {
        out.extend_from_slice(if x > 0.0 { b"\".inf\"" } else { b"\"-.inf\"" });
        return;
    }
    if x.is_sign_negative() {
        out.push(b'-');
    }
    let ax = x.abs();
    if ax == 0.0 {
        out.extend_from_slice(b"0.0");
        return;
    }
    // "d[.ddd]e<exp>": the shortest round-trip digits, exponent unpadded.
    // value = 0.digits × 10^k; the fpconv branches key on its own
    // K = k - ndigits and |exp| = |k - 1|
    let sci = format!("{:e}", ax);
    let (mantissa, exp_str) = sci
        .split_once('e')
        .expect("LowerExp always writes an exponent");
    let exp: i32 = exp_str.parse().expect("LowerExp exponent is an integer");
    let digits: Vec<u8> = mantissa.bytes().filter(|c| *c != b'.').collect();
    let ndigits = digits.len() as i32;
    let k = exp + 1; // the decimal point sits after k digits
    let fp_k = k - ndigits;
    if fp_k >= 0 && exp.abs() < 15 {
        out.extend_from_slice(&digits);
        for _ in 0..fp_k {
            out.push(b'0');
        }
        out.extend_from_slice(b".0");
        return;
    }
    if fp_k < 0 && (fp_k > -7 || exp.abs() < 10) {
        // the decimal point sits exactly k digits in
        if k <= 0 {
            out.extend_from_slice(b"0.");
            for _ in 0..(-k) {
                out.push(b'0');
            }
            out.extend_from_slice(&digits);
        } else {
            out.extend_from_slice(&digits[..k as usize]);
            out.push(b'.');
            out.extend_from_slice(&digits[k as usize..]);
        }
        return;
    }
    // scientific: d[.ddd]e±XX, two exponent digits minimum
    out.push(digits[0]);
    if ndigits > 1 {
        out.push(b'.');
        out.extend_from_slice(&digits[1..]);
    }
    out.push(b'e');
    out.push(if exp < 0 { b'-' } else { b'+' });
    let mut e = exp.unsigned_abs();
    let mut wrote_cent = false;
    if e > 99 {
        let cent = e / 100;
        out.push(b'0' + cent as u8);
        e -= cent * 100;
        wrote_cent = true;
    }
    if e > 9 {
        let dec = e / 10;
        out.push(b'0' + dec as u8);
        e -= dec * 10;
    } else if wrote_cent {
        out.push(b'0');
    }
    out.push(b'0' + (e % 10) as u8);
}

/// Deserialize `T` from JSON text (serde over the engine's JSON face).
pub fn from_str<T: DeserializeOwned>(s: &str) -> std::result::Result<T, crate::DeserError> {
    from_slice(s.as_bytes())
}

/// Byte form of [`from_str`].
pub fn from_slice<T: DeserializeOwned>(s: &[u8]) -> std::result::Result<T, crate::DeserError> {
    let value = parse_bytes(s)?;
    crate::de::from_value(&value)
}

/// Serialize `T` to canonical JSON text.
pub fn to_string<T: Serialize + ?Sized>(value: &T) -> Result<String> {
    serialize(&crate::ser::to_value(value).map_err(|_| ErrorKind::Arg)?)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The engine route (the DOM build + the engine's JSON emit) —
    /// the differential reference the writer is pinned against. The
    /// tagged fallback in [`serialize`] rides the same machinery.
    fn engine_serialize(value: &Value) -> Result<String> {
        let doc = emit::build_document(value)?;
        doc.serialize_json_string()
    }

    /// Byte equality of the writer route against the engine route:
    /// every accepted shape spells identically, every refusal carries
    /// the same kind.
    fn pin(value: Value) {
        let mine = serialize(&value);
        let engine = engine_serialize(&value);
        match (mine, engine) {
            (Ok(a), Ok(b)) => assert_eq!(a, b, "writer drift on {value:?}"),
            (Err(a), Err(b)) => assert_eq!(a.kind(), b.kind(), "error drift on {value:?}"),
            (a, b) => panic!(
                "verdict drift on {value:?}: writer {a:?} vs engine {:?}",
                b.map(|_| "ok".to_owned())
            ),
        }
    }

    #[test]
    fn writer_matches_engine_on_scalars_and_edges() {
        pin(Value::Null);
        pin(Value::Bool(true));
        pin(Value::Bool(false));
        pin(Value::Number(Number::I64(0)));
        pin(Value::Number(Number::I64(i64::MIN)));
        pin(Value::Number(Number::U64(u64::MAX)));
        pin(Value::String(String::new()));
        pin(Value::String("null".into()));
        pin(Value::String("a\"b\\c\n\t\u{1}\u{1f}".into()));
        pin(Value::String("héllo wörld ✓ 日本語 🎉".into()));
        pin(Value::Sequence(vec![]));
        pin(Value::Mapping(Mapping::new()));
        pin(Value::Sequence(vec![
            Value::Sequence(vec![]),
            Value::Mapping(Mapping::new()),
            Value::Number(Number::I64(-42)),
            Value::String("tab\tstr".into()),
        ]));
    }

    #[test]
    fn writer_matches_engine_on_floats() {
        // the fpconv shape's branch points, plus a shortest-digit
        // sample of common magnitudes
        let floats = [
            0.0,
            -0.0,
            1.0,
            -1.5,
            0.1,
            0.001,
            1e-5,
            1e-7,
            1e-8,
            1e-10,
            1e-14,
            1e-15,
            1e-16,
            1e14,
            1e15,
            1500000000000000.0,
            999999999999999.9,
            1e16,
            1e100,
            1e300,
            f64::MAX,
            f64::MIN_POSITIVE,
            f64::MIN_POSITIVE * 0.5,
            std::f64::consts::PI,
            -123456.789,
        ];
        for f in floats {
            pin(Value::Number(Number::F64(f)));
        }
        pin(Value::Number(Number::F64(f64::INFINITY)));
        pin(Value::Number(Number::F64(f64::NEG_INFINITY)));
        pin(Value::Number(Number::F64(f64::NAN)));
    }

    #[test]
    fn writer_matches_engine_on_keys() {
        let mut m = Mapping::new();
        m.insert(Value::String("k".into()), Value::Bool(true));
        m.insert(Value::Number(Number::I64(1)), Value::Bool(true));
        m.insert(Value::Number(Number::U64(7)), Value::Bool(true));
        m.insert(Value::Number(Number::F64(1.5)), Value::Bool(true));
        m.insert(Value::Null, Value::Bool(true));
        m.insert(Value::Bool(true), Value::Bool(true));
        m.insert(Value::String("a\"b".into()), Value::Bool(false));
        pin(Value::Mapping(m));
        // non-scalar keys: both routes refuse
        let mut bad = Mapping::new();
        bad.insert(
            Value::Sequence(vec![Value::Number(Number::I64(1))]),
            Value::Bool(true),
        );
        pin(Value::Mapping(bad));
        let mut bad2 = Mapping::new();
        bad2.insert(Value::Mapping(Mapping::new()), Value::Bool(true));
        pin(Value::Mapping(bad2));
    }

    #[test]
    fn writer_matches_engine_on_tagged() {
        pin(Value::Tagged(Box::new(crate::value::Tagged {
            tag: "tag:yaml.org,2002:int".into(),
            value: Value::Number(Number::I64(7)),
        })));
        pin(Value::Tagged(Box::new(crate::value::Tagged {
            tag: "x".into(),
            value: Value::Sequence(vec![Value::Number(Number::I64(1))]),
        })));
        pin(Value::Sequence(vec![Value::Tagged(Box::new(
            crate::value::Tagged {
                tag: "x".into(),
                value: Value::Number(Number::I64(1)),
            },
        ))]));
    }

    #[test]
    fn writer_matches_engine_on_depth() {
        let nest = |depth: usize| {
            let mut v = Value::Null;
            for _ in 0..depth {
                v = Value::Sequence(vec![v]);
            }
            v
        };
        pin(nest(4));
        pin(nest(999));
        pin(nest(1000));
        pin(nest(1500));
    }

    /// The canonical shape the face pins (tests/faces.rs carries the
    /// public twin of this).
    #[test]
    fn canonical_shape_holds() {
        let v = parse(r#"{"b": [1, 2], "a": null}"#).unwrap();
        assert_eq!(serialize(&v).unwrap(), "{\"b\": [1, 2], \"a\": null}\n");
    }

    /// Route equality over the edge corpus: the tape route's Value
    /// and the DOM route's Value agree, and the failures agree too
    /// (the fallback makes this near-tautological for errors, but the
    /// accepted set MUST go through the tape — assert the fast route
    /// is what answered by parsing the tape directly).
    #[test]
    fn tape_route_matches_dom_route() {
        let corpus = [
            r#"{"b": [1, 2], "a": null}"#,
            "42",
            "\"s\"",
            "true",
            "null",
            "1.5",
            "-0",
            "1e5",
            "18446744073709551615",
            "18446744073709551616",
            "[]",
            "{}",
            r#""Aé😀""#,
            r#""\/\b\f\n\r\t""#,
            r#""\ud800""#,
            r#""\udc00""#,
            r#""😀""#,
            "\"\"",
            r#"" ""#,
            r#""a
b""#,
            "[0.1, 1e999, -1e999, 1E+2, 0.0, -0.0, 123456789012345678901234567890]",
            "[\"héllo\", \"日本語\", \"emoji 🎉\", \"\\u65e5\"]",
            r#"{"a": 1, "a": 2}"#,
            "[NaN, Infinity, .inf]",
            "\u{feff}{\"a\": 1}",
            "",
            "  \n\t ",
            r#"{"k": [1,}"#,
            r#"{"users":[{"id":1,"name":"U","prefs":{"dark":true,"n":0.5}}]}"#,
        ];
        for input in corpus {
            let dom = Document::parse_json(input.as_bytes()).and_then(|d| Value::from_document(&d));
            // every input the DOM face accepts must ride the TAPE
            // itself — a tape rejection here would mean the fast
            // route silently demoted to the fallback
            if dom.is_ok() && !input.contains('\u{feff}') {
                let fast = parse_tape(input.as_bytes());
                assert!(
                    fast.is_ok(),
                    "the tape route should carry {input:?}: {:?}",
                    fast.as_ref().map_err(|e| e.to_string())
                );
            }
            match parse(input) {
                Ok(v) => assert_eq!(
                    Some(v),
                    dom.ok(),
                    "tape route drifted from the DOM route on {input:?}"
                ),
                Err(e) => {
                    let err = dom.expect_err("tape refused what the DOM face accepts");
                    assert_eq!(e.kind(), err.kind(), "kind drift on {input:?}");
                    assert_eq!(e.message(), err.message(), "message drift on {input:?}");
                }
            }
        }
    }

    /// The deep shapes: 256 rides the tape, 257+ both refuse (the DOM
    /// face carries the same walk cap).
    #[test]
    fn depth_parity_holds() {
        for depth in [255usize, 256, 257, 1000] {
            let deep = format!("{}{}", "[".repeat(depth), "]".repeat(depth));
            let dom = Document::parse_json(deep.as_bytes()).and_then(|d| Value::from_document(&d));
            match parse(&deep) {
                Ok(v) => assert_eq!(Some(v), dom.ok(), "depth {depth} drifted"),
                Err(e) => {
                    let err = dom.expect_err("tape refused what the DOM face accepts");
                    assert_eq!(e.kind(), err.kind(), "depth {depth} kind drift");
                    assert_eq!(e.message(), err.message(), "depth {depth} message drift");
                }
            }
        }
    }

    /// The simple-key law rides the fallback: >1024-character keys
    /// carry the DOM face's exact error, at and past the boundary.
    #[test]
    fn simple_key_law_rides_the_fallback() {
        for key_len in [1022usize, 1023, 2000] {
            let input = format!("{{\"{}\": 1}}", "k".repeat(key_len));
            let dom =
                Document::parse_json(input.as_bytes()).and_then(|d| Value::from_document(&d));
            match parse(&input) {
                Ok(v) => assert_eq!(Some(v), dom.ok(), "key{key_len} drifted"),
                Err(e) => {
                    let err = dom.expect_err("tape refused what the DOM face accepts");
                    assert_eq!(e.kind(), err.kind());
                    assert_eq!(e.message(), err.message());
                }
            }
        }
    }

    /// A multibyte key at the byte gate: the law counts characters,
    /// not bytes (the round-108 fuzz shape).
    #[test]
    fn key_law_counts_chars_not_bytes() {
        // 400 × 'é' = 400 chars / 800 bytes: over the byte gate,
        // under the char budget — both faces accept
        let input = format!("{{\"{}\": 1}}", "é".repeat(400));
        let dom = Document::parse_json(input.as_bytes()).and_then(|d| Value::from_document(&d));
        assert!(dom.is_ok(), "both faces accept a 400-char multibyte key");
        assert_eq!(parse(&input).unwrap(), dom.unwrap());
        // 1100 × 'é' = 1100 chars / 2200 bytes: over both — refused
        let input = format!("{{\"{}\": 1}}", "é".repeat(1100));
        let dom = Document::parse_json(input.as_bytes());
        assert!(dom.is_err());
        assert!(parse(&input).is_err());
    }
}

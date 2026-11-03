//! The `serde_yaml`-shaped data model (teptris's proposal): a
//! fully-owned tree resolved from the engine's zero-copy DOM, with
//! tags kept as [`Value::Tagged`] and aliases expanded at load.
//!
//! Deliberate divergence from serde_yaml, documented: YAML 1.2 has no
//! merge keys, so `<<` stays a literal mapping key (the engine's
//! core-12 default — Psych/serde_yaml expand it; we follow the spec).

use crate::{ffi, Document, Error, ErrorKind, Node, NodeKind, Result, Style};
use std::borrow::Cow;
use std::collections::HashSet;
use std::fmt;

/// A YAML value: the `serde_yaml::Value` shape (minus `Number`'s
/// arbitrary-precision backend — the engine resolves i64/u64/f64).
#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    Null,
    Bool(bool),
    Number(Number),
    String(String),
    Sequence(Vec<Value>),
    Mapping(Mapping),
    Tagged(Box<Tagged>),
}

#[derive(Clone, Debug, PartialEq)]
pub struct Tagged {
    pub tag: String,
    pub value: Value,
}

/// Insertion-ordered mapping. Keys repeat when the source document
/// repeats them (the engine keeps last-value-wins at parse; the tree
/// preserves what the DOM reports per pair).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Mapping(pub Vec<(Value, Value)>);

impl Mapping {
    pub fn new() -> Mapping {
        Mapping(Vec::new())
    }
    pub fn get(&self, key: &Value) -> Option<&Value> {
        self.0.iter().rev().find(|(k, _)| k == key).map(|(_, v)| v)
    }
    pub fn insert(&mut self, key: Value, value: Value) {
        if let Some(slot) = self.0.iter_mut().rev().find(|(k, _)| *k == key) {
            slot.1 = value;
        } else {
            self.0.push((key, value));
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Number {
    I64(i64),
    U64(u64),
    F64(f64),
}

impl Value {
    /// Materialize a whole document (its first root). Errors on an
    /// empty stream.
    pub fn from_document(doc: &Document<'_>) -> Result<Value> {
        let root = doc
            .first_root()
            .ok_or(ErrorKind::Parse)
            .map_err(crate::Error::from)?;
        Value::from_root(root)
    }

    /// Materialize one document root (the stream face's per-document
    /// leg): an empty root — the engine's synthesized empty scalar
    /// for a content-less document — resolves to [`Value::Null`].
    pub fn from_root(root: Node<'_>) -> Result<Value> {
        let mut visiting = HashSet::new();
        Value::from_node_inner(root, &mut visiting)
    }

    fn from_node_inner(node: Node<'_>, visiting: &mut HashSet<u32>) -> Result<Value> {
        let tagged = |v: Value, node: &Node<'_>| {
            match node.tag() {
                Some(bytes) => match std::str::from_utf8(bytes) {
                    Ok(t) => Value::Tagged(Box::new(Tagged {
                        tag: t.to_owned(),
                        value: v,
                    })),
                    // DOM tags are engine-validated UTF-8; unreachable
                    // in practice, degrade to the untagged value.
                    Err(_) => v,
                },
                None => v,
            }
        };
        match node.kind() {
            NodeKind::Scalar => Ok(tagged(scalar_to_value(resolve_scalar(node)), &node)),
            NodeKind::Sequence => {
                let mut items = Vec::with_capacity(node.seq_count());
                for i in 0..node.seq_count() {
                    items.push(Value::from_node_inner(
                        node.seq_at(i)
                            .ok_or(ErrorKind::Internal)
                            .map_err(crate::Error::from)?,
                        visiting,
                    )?);
                }
                Ok(tagged(Value::Sequence(items), &node))
            }
            NodeKind::Mapping => {
                let mut map = Mapping::new();
                for i in 0..node.map_count() {
                    let (k, v) = node
                        .map_at(i)
                        .ok_or(ErrorKind::Internal)
                        .map_err(crate::Error::from)?;
                    let key = Value::from_node_inner(k, visiting)?;
                    let value = Value::from_node_inner(v, visiting)?;
                    map.insert(key, value);
                }
                Ok(tagged(Value::Mapping(map), &node))
            }
            NodeKind::Alias => {
                let target = node
                    .alias_target()
                    .ok_or(ErrorKind::Parse)
                    .map_err(crate::Error::from)?;
                let id = target.id();
                if !visiting.insert(id) {
                    // The cycle guard pops on the way out; a hit here
                    // means the target is still being expanded.
                    return Err(ErrorKind::Parse.into());
                }
                let v = Value::from_node_inner(target, visiting);
                visiting.remove(&id);
                Ok(v?)
            }
        }
    }
}

/// The resolved shape of one scalar, borrowing the document where the
/// content is valid UTF-8. The SSOT for scalar typing: the owned
/// [`Value`] walk (SSOT consumer: `from_node_inner`) and the Node
/// serde route (`de_node`) both read through it.
pub(crate) enum ScalarVal<'a> {
    Null,
    Bool(bool),
    Int(i64),
    Uint(u64),
    Float(f64),
    Str(Cow<'a, str>),
}

/// The style gate + core-12 resolution for one scalar node. ANY is the
/// engine's recorded style for the SYNTHESIZED empty scalar (a
/// content-less document, an empty `a:` value) — it is empty content,
/// so it resolves by the schema like a plain scalar would (empty →
/// Null, the nil-placement law; quoted empties keep their real style
/// and stay strings).
pub(crate) fn resolve_scalar(node: Node<'_>) -> ScalarVal<'_> {
    let bytes = node.value().unwrap_or_default();
    let style = node.style();
    if style == Style::Plain || style == Style::Any {
        resolve_plain_ref(bytes)
    } else {
        ScalarVal::Str(Cow::Owned(String::from_utf8_lossy(bytes).into_owned()))
    }
}

pub(crate) fn scalar_to_value(s: ScalarVal<'_>) -> Value {
    match s {
        ScalarVal::Null => Value::Null,
        ScalarVal::Bool(b) => Value::Bool(b),
        ScalarVal::Int(v) => Value::Number(Number::I64(v)),
        ScalarVal::Uint(v) => Value::Number(Number::U64(v)),
        ScalarVal::Float(v) => Value::Number(Number::F64(v)),
        ScalarVal::Str(Cow::Borrowed(s)) => Value::String(s.to_owned()),
        ScalarVal::Str(Cow::Owned(s)) => Value::String(s),
    }
}

/// YAML 1.2 core-schema resolution for PLAIN scalars: null, bool,
/// int (dec / `0o` / `0x`), float (`.inf`/`.nan`/decimal) — else a
/// string. An empty plain scalar is null. Borrowing form of
/// [`resolve_plain`].
pub(crate) fn resolve_plain_ref(bytes: &[u8]) -> ScalarVal<'_> {
    let s = match std::str::from_utf8(bytes) {
        Ok(s) => s,
        Err(_) => return ScalarVal::Str(Cow::Owned(String::from_utf8_lossy(bytes).into_owned())),
    };
    match s {
        "" | "~" | "null" | "Null" | "NULL" => return ScalarVal::Null,
        "true" | "True" | "TRUE" => return ScalarVal::Bool(true),
        "false" | "False" | "FALSE" => return ScalarVal::Bool(false),
        ".inf" | ".Inf" | ".INF" | "+.inf" | "+.Inf" | "+.INF" => {
            return ScalarVal::Float(f64::INFINITY)
        }
        "-.inf" | "-.Inf" | "-.INF" => return ScalarVal::Float(f64::NEG_INFINITY),
        ".nan" | ".NaN" | ".NAN" => return ScalarVal::Float(f64::NAN),
        _ => {}
    }
    if let Some(n) = parse_yaml_int(s) {
        return match n {
            Number::I64(v) => ScalarVal::Int(v),
            Number::U64(v) => ScalarVal::Uint(v),
            Number::F64(_) => unreachable!("parse_yaml_int returns integral numbers"),
        };
    }
    if let Some(Number::F64(f)) = parse_yaml_float(s) {
        return ScalarVal::Float(f);
    }
    ScalarVal::Str(Cow::Borrowed(s))
}

/// YAML 1.2 core-schema resolution for PLAIN scalars: null, bool,
/// int (dec / `0o` / `0x`), float (`.inf`/`.nan`/decimal) — else a
/// string. An empty plain scalar is null.
pub(crate) fn resolve_plain(bytes: &[u8]) -> Value {
    scalar_to_value(resolve_plain_ref(bytes))
}

fn digits_only(s: &str) -> bool {
    !s.is_empty() && s.bytes().all(|b| b.is_ascii_digit())
}

fn parse_yaml_int(s: &str) -> Option<Number> {
    let (sign, body) = match s.strip_prefix('-') {
        Some(b) => (-1i128, b),
        None => (1i128, s.strip_prefix('+').unwrap_or(s)),
    };
    if body.is_empty() {
        return None;
    }
    if let Some(hex) = body.strip_prefix("0x") {
        return u64::from_str_radix(hex, 16)
            .ok()
            .and_then(|m| i64::try_from(sign * i128::from(m)).ok().map(Number::I64));
    }
    if let Some(oct) = body.strip_prefix("0o") {
        return u64::from_str_radix(oct, 8)
            .ok()
            .and_then(|m| i64::try_from(sign * i128::from(m)).ok().map(Number::I64));
    }
    if !digits_only(body) {
        return None;
    }
    // serde_yaml: i64 first, u64 for the positive overflow window
    if let Ok(v) = s.parse::<i64>() {
        return Some(Number::I64(v));
    }
    s.parse::<u64>().ok().map(Number::U64)
}

fn parse_yaml_float(s: &str) -> Option<Number> {
    let body = s.strip_prefix(['-', '+']).unwrap_or(s);
    if body.is_empty() || !body.bytes().any(|b| b == b'.' || b == b'e' || b == b'E') {
        return None; // a bare digit run is an int, not a float
    }
    let mut seen_dot = false;
    let mut seen_exp = false;
    let bytes = body.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        let b = bytes[i];
        match b {
            b'0'..=b'9' => i += 1,
            b'.' if !seen_dot && !seen_exp => {
                seen_dot = true;
                i += 1;
            }
            b'e' | b'E' if !seen_exp && i + 1 < bytes.len() => {
                seen_exp = true;
                i += 1;
                if bytes[i] == b'-' || bytes[i] == b'+' {
                    i += 1;
                }
            }
            _ => return None,
        }
    }
    // at least one digit must exist (reject ".", "e5")
    if !bytes.iter().any(|b| b.is_ascii_digit()) {
        return None;
    }
    s.parse::<f64>().ok().map(Number::F64)
}

impl fmt::Display for Number {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Number::I64(v) => write!(f, "{}", v),
            Number::U64(v) => write!(f, "{}", v),
            // serde_yaml prints floats through Rust's shortest form
            Number::F64(v) => write!(f, "{}", v),
        }
    }
}

/// Every document in the stream, in order (the load_all law, the
/// ruby/py/ts shape at rs parity). One parse, one walk of the engine's
/// DOM forest; the boundary placement is the other runtimes' law:
///
/// * single-document input yields the length-1 vec;
/// * an empty document (a bare `---`, a comment-only document) rides
///   its boundary as [`Value::Null`] — the engine synthesizes the
///   empty scalar and the resolver makes it null;
/// * an empty stream (no documents at all) yields the empty vec — the
///   engine's NULL-document-and-OK verdict, not an error;
/// * a malformed document fails the whole parse, and the error
///   carries the engine's message with its line/column.
///
/// Eager [`Vec`] by choice (yeptris#886 slice 1): the engine's
/// `yeptris_parse` returns the whole forest in one call, so there is
/// nothing to defer — the lazy per-document form is iterparse's shape
/// ([`crate::event::Puller`] drives the same engine event-at-a-time).
pub fn parse_stream(input: &[u8]) -> Result<Vec<Value>> {
    let mut status = ffi::YEPTRIS_OK;
    let raw = unsafe {
        ffi::yeptris_parse(
            input.as_ptr() as *const std::os::raw::c_char,
            input.len(),
            &mut status,
        )
    };
    if raw.is_null() {
        if status == ffi::YEPTRIS_OK {
            return Ok(Vec::new()); // the engine's empty-stream verdict
        }
        return Err(Error::from_status(status));
    }
    let doc = Document::from_raw(raw, input);
    let count = doc.document_count();
    let mut docs = Vec::with_capacity(count);
    for i in 0..count {
        let root = doc
            .root(i)
            .ok_or(ErrorKind::Internal)
            .map_err(crate::Error::from)?;
        docs.push(Value::from_root(root)?);
    }
    Ok(docs)
}

/// One failed document of the tolerant walk ([`each_document`]): the
/// failed document's OWN geometry (each frame parse starts at the
/// frame, so the engine's line/col are the failed document's) and its
/// 0-based stream index — failures consume an index, so the pair
/// order is the stream's.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StreamDocError {
    pub index: usize,
    pub line: u32,
    pub col: u32,
    pub kind: ErrorKind,
}

impl fmt::Display for StreamDocError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "stream document {}: at line {}, column {}",
            self.index, self.line, self.col
        )
    }
}

/// The tolerant per-document walk (engine #921, rs#45): a lazy
/// iterator of `Result<Value, StreamDocError>` in stream order —
/// `Ok(value)` per valid document, `Err` per failed one. A
/// parse-class failure carries the failed document's own line/col and
/// its stream index, and the walk CONTINUES at the next line-start
/// marker (the valid documents after a bad one are kept —
/// [`parse_stream`] fails the whole stream instead). The stream-fatal
/// classes (memory/encoding) yield their error and end the walk; an
/// unrecoverable tail ends it. Nothing parses until iteration starts.
/// Frames materialize through the same DOM route as
/// [`parse_stream`]: one typing SSOT.
///
/// Requires libyeptris with the stream iterator (0.6.60+); on an
/// older engine the FIRST iteration errors with
/// [`ErrorKind::Unsupported`] — a link-time failure otherwise (the
/// symbols are unresolved), which is why this is a runtime check.
pub fn each_document(input: &[u8]) -> EachDocument<'_> {
    let raw = unsafe {
        ffi::yeptris_stream_iter_new(
            input.as_ptr() as *const std::os::raw::c_char,
            input.len(),
            std::ptr::null(),
        )
    };
    EachDocument {
        raw,
        input,
        done: raw.is_null(),
    }
}

/// The lazy iterator [`each_document`] returns (see its contract).
pub struct EachDocument<'a> {
    raw: *mut std::os::raw::c_void,
    input: &'a [u8],
    done: bool,
}

impl Drop for EachDocument<'_> {
    fn drop(&mut self) {
        if !self.raw.is_null() {
            unsafe { ffi::yeptris_stream_iter_free(self.raw) };
        }
    }
}

impl Iterator for EachDocument<'_> {
    type Item = std::result::Result<Value, StreamDocError>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.done || self.raw.is_null() {
            self.done = true;
            return None;
        }
        // one engine step per item: the iterator resyncs INTERNALLY
        // (a parse-class failure returns with the walk already landed
        // on the next marker), so this body never spins
        let mut doc: ffi::YeptrisDocument = std::ptr::null_mut();
        let mut index: usize = 0;
        let st = unsafe { ffi::yeptris_stream_iter_next(self.raw, &mut doc, &mut index) };
        {
            if st == ffi::YEPTRIS_OK {
                if doc.is_null() {
                    self.done = true; // the stream is over
                    return None;
                }
                // materialize through the DOM route (the typing SSOT),
                // then free the frame — the Value is fully owned
                let frame = Document::from_raw(doc, self.input);
                let value = match frame.first_root() {
                    Some(root) => Value::from_root(root),
                    None => Ok(Value::Null), // a bare marker rides Null
                };
                match value {
                    Ok(v) => return Some(Ok(v)),
                    Err(e) => {
                        // the framing accepted what materialization
                        // refused: report as the failed document
                        let (line, col) = self.status();
                        return Some(Err(StreamDocError {
                            index,
                            line,
                            col,
                            kind: e.kind(),
                        }));
                    }
                }
            }
            let (line, col) = self.status();
            let kind = match st {
                ffi::YEPTRIS_ERROR_MEMORY => ErrorKind::Memory,
                ffi::YEPTRIS_ERROR_DEPTH => ErrorKind::Depth,
                ffi::YEPTRIS_ERROR_ENCODING => ErrorKind::Encoding,
                _ => ErrorKind::Parse,
            };
            if st == ffi::YEPTRIS_ERROR_MEMORY || st == ffi::YEPTRIS_ERROR_ENCODING {
                self.done = true; // stream-fatal: the iterator is done
            }
            return Some(Err(StreamDocError {
                index,
                line,
                col,
                kind,
            }));
        }
    }
}

impl EachDocument<'_> {
    fn status(&self) -> (u32, u32) {
        let mut line: u32 = 0;
        let mut col: u32 = 0;
        unsafe {
            ffi::yeptris_stream_iter_status(self.raw, &mut line, &mut col);
        }
        (line, col)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn resolve(s: &str) -> Value {
        resolve_plain(s.as_bytes())
    }

    #[test]
    fn null_words() {
        for s in ["", "~", "null", "Null", "NULL"] {
            assert_eq!(resolve(s), Value::Null, "{}", s);
        }
    }

    #[test]
    fn bools() {
        assert_eq!(resolve("true"), Value::Bool(true));
        assert_eq!(resolve("FALSE"), Value::Bool(false));
        assert_eq!(resolve("True"), Value::Bool(true));
        assert_eq!(resolve("yes"), Value::String("yes".into())); // 1.2: not a bool
    }

    #[test]
    fn ints() {
        assert_eq!(resolve("0"), Value::Number(Number::I64(0)));
        assert_eq!(resolve("-42"), Value::Number(Number::I64(-42)));
        assert_eq!(resolve("+7"), Value::Number(Number::I64(7)));
        assert_eq!(resolve("0x1f"), Value::Number(Number::I64(31)));
        assert_eq!(resolve("0o17"), Value::Number(Number::I64(15)));
        assert_eq!(
            resolve("18446744073709551615"),
            Value::Number(Number::U64(u64::MAX))
        );
        assert_eq!(resolve("1_000"), Value::String("1_000".into())); // 1.2 core: no underscores
    }

    #[test]
    fn floats() {
        assert_eq!(resolve("1.5"), Value::Number(Number::F64(1.5)));
        assert_eq!(resolve("-1e3"), Value::Number(Number::F64(-1000.0)));
        assert_eq!(resolve("12.00"), Value::Number(Number::F64(12.0)));
        assert_eq!(resolve(".inf"), Value::Number(Number::F64(f64::INFINITY)));
        assert!(matches!(resolve(".nan"), Value::Number(Number::F64(n)) if n.is_nan()));
        assert_eq!(resolve("."), Value::String(".".into()));
        assert_eq!(resolve("1.2.3"), Value::String("1.2.3".into()));
    }

    #[test]
    fn non_plain_stays_string() {
        // exercised through Document in the integration tests; the
        // style gate lives in from_node_inner
    }
}

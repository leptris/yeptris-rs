//! Idiomatic Rust bindings for [libyeptris](https://github.com/leptris/yeptris),
//! a pure-C11 YAML 1.2 parser with a document arena, zero-copy value
//! views, and a byte-stable emitter.
//!
//! ```no_run
//! use yeptris::Document;
//!
//! let doc = Document::parse(b"name: yeptris\nlangs: [c, rust]\n").unwrap();
//! let root = doc.first_root().unwrap();
//! assert_eq!(root.get("name").unwrap().str(), Some("yeptris"));
//! assert_eq!(doc.serialize_string().unwrap(), "name: yeptris\nlangs:\n- c\n- rust\n");
//! ```
//!
//! The C library is resolved at link time: set `YEPTRIS_LIB_PATH`
//! (see `build.rs`) or install libyeptris on the library path.

mod ffi;

use std::ffi::{c_char, CStr};
use std::fmt;
use std::marker::PhantomData;

/// Error surfaced by every fallible binding call. Mirrors the
/// `YeptrisStatus` codes from the C API.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    Parse,
    Memory,
    Depth,
    Encoding,
    Io,
    Arg,
    Unsupported,
    Internal,
    Schema,
    Unknown(i32),
}

impl Error {
    fn from_status(status: i32) -> Error {
        match status {
            ffi::YEPTRIS_ERROR_PARSE => Error::Parse,
            ffi::YEPTRIS_ERROR_MEMORY => Error::Memory,
            ffi::YEPTRIS_ERROR_DEPTH => Error::Depth,
            ffi::YEPTRIS_ERROR_ENCODING => Error::Encoding,
            ffi::YEPTRIS_ERROR_IO => Error::Io,
            ffi::YEPTRIS_ERROR_ARG => Error::Arg,
            ffi::YEPTRIS_ERROR_UNSUPPORTED => Error::Unsupported,
            ffi::YEPTRIS_ERROR_INTERNAL => Error::Internal,
            ffi::YEPTRIS_ERROR_SCHEMA => Error::Schema,
            other => Error::Unknown(other),
        }
    }

    /// The engine's message for the most recent failing call on this
    /// thread (with its line/column folded in when the engine
    /// reported one). Valid only immediately after the failure.
    pub fn message(&self) -> String {
        let mut line: u32 = 0;
        let mut col: u32 = 0;
        let msg = unsafe { ffi::yeptris_last_error(&mut line, &mut col) };
        let text = if msg.is_null() {
            format!("{:?}", self)
        } else {
            unsafe { CStr::from_ptr(msg) }
                .to_string_lossy()
                .into_owned()
        };
        if line != 0 {
            format!("{} (line {}, column {})", text, line, col)
        } else {
            text
        }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.message())
    }
}

impl std::error::Error for Error {}

pub type Result<T> = std::result::Result<T, Error>;

/// The engine version string (`yeptris_version()`).
pub fn version() -> &'static str {
    unsafe {
        let v = ffi::yeptris_version();
        if v.is_null() {
            ""
        } else {
            CStr::from_ptr(v).to_str().unwrap_or("")
        }
    }
}

/// A parsed YAML stream. Owns the C document set; freed on drop.
///
/// One `Document` may hold several documents from a `---` stream —
/// see [`Document::document_count`] and [`Document::root`].
///
/// The `'input` parameter is the contract the engine's zero-copy
/// design imposes: value views point INTO the caller's buffer, so a
/// `Document` cannot outlive the bytes it parsed.
pub struct Document<'input> {
    raw: ffi::YeptrisDocument,
    _input: PhantomData<&'input [u8]>,
}

impl Document<'_> {
    /// Parse YAML from bytes. Input must be UTF-8 (the engine
    /// validates and rejects ill-formed input with [`Error::Encoding`]).
    /// The engine borrows the buffer — hold it while the `Document`
    /// lives.
    pub fn parse<'a>(input: &'a [u8]) -> Result<Document<'a>> {
        let mut status: ffi::YeptrisStatus = ffi::YEPTRIS_OK;
        let raw = unsafe {
            ffi::yeptris_parse(input.as_ptr() as *const c_char, input.len(), &mut status)
        };
        if raw.is_null() {
            return Err(Error::from_status(status));
        }
        Ok(Document {
            raw,
            _input: PhantomData,
        })
    }

    /// Number of documents in the parsed stream.
    pub fn document_count(&self) -> usize {
        unsafe { ffi::yeptris_document_count(self.raw) }
    }

    /// Root node of document `index` (0-based).
    pub fn root(&self, index: usize) -> Option<Node<'_>> {
        let raw = unsafe { ffi::yeptris_document_root(self.raw, index) };
        if raw.is_null() {
            None
        } else {
            Some(Node {
                raw,
                _doc: PhantomData,
            })
        }
    }

    /// Root node of the first document.
    pub fn first_root(&self) -> Option<Node<'_>> {
        self.root(0)
    }

    /// Serialize the whole stream to owned bytes (the emitter's
    /// canonical, byte-stable form).
    pub fn serialize(&self) -> Result<Vec<u8>> {
        let mut len: usize = 0;
        let raw = unsafe { ffi::yeptris_serialize(self.raw, &mut len) };
        if raw.is_null() {
            return Err(Error::Memory);
        }
        let out = unsafe { std::slice::from_raw_parts(raw as *const u8, len) }.to_vec();
        unsafe { ffi::yeptris_free(raw as *mut core::ffi::c_void) };
        Ok(out)
    }

    /// Serialize to an owned UTF-8 string.
    pub fn serialize_string(&self) -> Result<String> {
        let bytes = self.serialize()?;
        String::from_utf8(bytes).map_err(|_| Error::Internal)
    }
}

impl Drop for Document<'_> {
    fn drop(&mut self) {
        unsafe { ffi::yeptris_document_free(self.raw) };
    }
}

impl fmt::Debug for Document<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Document({} docs)", self.document_count())
    }
}

/// The kind of a node.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NodeKind {
    Scalar,
    Sequence,
    Mapping,
    Alias,
}

/// A scalar's recorded presentation style.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Style {
    Any,
    Plain,
    SingleQuoted,
    DoubleQuoted,
    Literal,
    Folded,
}

impl Style {
    fn from_raw(raw: ffi::YeptrisScalarStyle) -> Style {
        match raw {
            ffi::YEPTRIS_STYLE_PLAIN => Style::Plain,
            ffi::YEPTRIS_STYLE_SINGLE_QUOTED => Style::SingleQuoted,
            ffi::YEPTRIS_STYLE_DOUBLE_QUOTED => Style::DoubleQuoted,
            ffi::YEPTRIS_STYLE_LITERAL => Style::Literal,
            ffi::YEPTRIS_STYLE_FOLDED => Style::Folded,
            _ => Style::Any,
        }
    }
}

/// A node, borrowed from its document for its whole lifetime.
///
/// Value bytes are zero-copy views into the document's arena — the
/// lifetime parameter enforces what the C contract states (valid
/// until `yeptris_document_free`).
#[derive(Clone, Copy)]
pub struct Node<'doc> {
    raw: ffi::YeptrisNode,
    _doc: PhantomData<&'doc Document<'doc>>,
}

impl<'doc> Node<'doc> {
    fn view(&self, ptr: *const c_char, len: usize) -> Option<&'doc [u8]> {
        if ptr.is_null() {
            None
        } else {
            Some(unsafe { std::slice::from_raw_parts(ptr as *const u8, len) })
        }
    }

    /// The node's kind.
    pub fn kind(&self) -> NodeKind {
        match unsafe { ffi::yeptris_node_kind(self.raw) } {
            ffi::YEPTRIS_NODE_SEQUENCE => NodeKind::Sequence,
            ffi::YEPTRIS_NODE_MAPPING => NodeKind::Mapping,
            ffi::YEPTRIS_NODE_ALIAS => NodeKind::Alias,
            _ => NodeKind::Scalar,
        }
    }

    /// Stable identity within the document (equality only — not an
    /// index into any structure).
    pub fn id(&self) -> u32 {
        unsafe { ffi::yeptris_node_id(self.raw) }
    }

    /// Scalar content or alias name, as raw bytes. `None` for
    /// collections; an empty scalar reads as an empty slice (the
    /// engine stores its empty value slot as NULL).
    pub fn value(&self) -> Option<&'doc [u8]> {
        match self.kind() {
            NodeKind::Sequence | NodeKind::Mapping => return None,
            _ => {}
        }
        let mut len: usize = 0;
        let ptr = unsafe { ffi::yeptris_node_value(self.raw, &mut len) };
        if ptr.is_null() {
            return Some(&[]);
        }
        self.view(ptr, len)
    }

    /// Scalar content or alias name as UTF-8. `None` for collections.
    pub fn str(&self) -> Option<&'doc str> {
        self.value().and_then(|b| std::str::from_utf8(b).ok())
    }

    /// The scalar's recorded presentation style.
    pub fn style(&self) -> Style {
        Style::from_raw(unsafe { ffi::yeptris_node_style(self.raw) })
    }

    /// The node's tag, when one was present.
    pub fn tag(&self) -> Option<&'doc [u8]> {
        let mut len: usize = 0;
        let ptr = unsafe { ffi::yeptris_node_tag(self.raw, &mut len) };
        self.view(ptr, len)
    }

    /// The node's anchor, when one was present.
    pub fn anchor(&self) -> Option<&'doc [u8]> {
        let mut len: usize = 0;
        let ptr = unsafe { ffi::yeptris_node_anchor(self.raw, &mut len) };
        self.view(ptr, len)
    }

    /// The node this alias resolves to, if it resolved.
    pub fn alias_target(&self) -> Option<Node<'doc>> {
        let raw = unsafe { ffi::yeptris_node_alias_target(self.raw) };
        if raw.is_null() {
            None
        } else {
            Some(Node {
                raw,
                _doc: PhantomData,
            })
        }
    }

    /// Number of items in a sequence.
    pub fn seq_count(&self) -> usize {
        unsafe { ffi::yeptris_node_seq_count(self.raw) }
    }

    /// Item `index` of a sequence.
    pub fn seq_at(&self, index: usize) -> Option<Node<'doc>> {
        let raw = unsafe { ffi::yeptris_node_seq_at(self.raw, index) };
        if raw.is_null() {
            None
        } else {
            Some(Node {
                raw,
                _doc: PhantomData,
            })
        }
    }

    /// Iterate a sequence's items.
    pub fn items(&self) -> NodeChildren<'doc> {
        NodeChildren {
            parent: *self,
            index: 0,
            count: self.seq_count(),
        }
    }

    /// Number of entries in a mapping.
    pub fn map_count(&self) -> usize {
        unsafe { ffi::yeptris_node_map_count(self.raw) }
    }

    /// Value for string key `key` in a mapping.
    pub fn get(&self, key: &str) -> Option<Node<'doc>> {
        let raw = unsafe {
            ffi::yeptris_node_map_get(self.raw, key.as_ptr() as *const c_char, key.len())
        };
        if raw.is_null() {
            None
        } else {
            Some(Node {
                raw,
                _doc: PhantomData,
            })
        }
    }

    /// Key/value pair `index` of a mapping.
    pub fn map_at(&self, index: usize) -> Option<(Node<'doc>, Node<'doc>)> {
        let mut key: ffi::YeptrisNode = std::ptr::null_mut();
        let mut value: ffi::YeptrisNode = std::ptr::null_mut();
        let ok = unsafe { ffi::yeptris_node_map_at(self.raw, index, &mut key, &mut value) };
        if ok != 0 || key.is_null() || value.is_null() {
            return None;
        }
        Some((
            Node {
                raw: key,
                _doc: PhantomData,
            },
            Node {
                raw: value,
                _doc: PhantomData,
            },
        ))
    }

    /// Iterate a mapping's (key, value) entries.
    pub fn entries(&self) -> MapEntries<'doc> {
        MapEntries {
            parent: *self,
            index: 0,
            count: self.map_count(),
        }
    }
}

impl PartialEq for Node<'_> {
    fn eq(&self, other: &Self) -> bool {
        self.id() == other.id()
    }
}

impl fmt::Debug for Node<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.kind() {
            NodeKind::Scalar => write!(f, "Scalar({:?})", self.str().unwrap_or("")),
            NodeKind::Sequence => write!(f, "Sequence({} items)", self.seq_count()),
            NodeKind::Mapping => write!(f, "Mapping({} entries)", self.map_count()),
            NodeKind::Alias => write!(f, "Alias({:?})", self.str().unwrap_or("")),
        }
    }
}

/// Iterator over a sequence's items.
pub struct NodeChildren<'doc> {
    parent: Node<'doc>,
    index: usize,
    count: usize,
}

impl<'doc> Iterator for NodeChildren<'doc> {
    type Item = Node<'doc>;

    fn next(&mut self) -> Option<Node<'doc>> {
        if self.index >= self.count {
            return None;
        }
        let item = self.parent.seq_at(self.index);
        self.index += 1;
        item
    }
}

/// Iterator over a mapping's (key, value) entries.
pub struct MapEntries<'doc> {
    parent: Node<'doc>,
    index: usize,
    count: usize,
}

impl<'doc> Iterator for MapEntries<'doc> {
    type Item = (Node<'doc>, Node<'doc>);

    fn next(&mut self) -> Option<Self::Item> {
        if self.index >= self.count {
            return None;
        }
        let entry = self.parent.map_at(self.index);
        self.index += 1;
        entry
    }
}

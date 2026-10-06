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

/// The compiled-plan columnar walk (yeptris/plan.h).
///
/// A plan is compiled once from a strict-JSON spec and applied to a
/// [`Document`] in a single C pass, producing typed columns:
///
/// ```
/// use yeptris::plan::Plan;
///
/// let doc = yeptris::Document::parse(b"items:\n  - {id: 7, tag: x}\n").unwrap();
/// let plan = Plan::compile(
///     r#"{"kind":"map","path":"items","children":[
///         {"name":"id","kind":"int"},{"name":"tag","kind":"str"}]}"#,
/// ).unwrap();
/// let cols = doc.plan_walk(&plan).unwrap();
/// assert_eq!(cols.ints(0).unwrap(), &[7]);
/// ```
///
/// The spec's leaf kinds are `int`, `float`, `str`, `bool`; `kind`
/// is `"seq"` or `"map"` and `path` selects the rows container (a
/// string, or an array of strings for a segmented path). Explicit
/// null and missing leaves mark the slot null. BOOL columns read
/// through [`PlanColumns::ints`] as 0/1, matching the engine's
/// columnar contract.
pub mod plan {
    use crate::ffi;
    use crate::{Document, Error};

    /// A compiled plan. Owns the engine's plan handle; freed on drop.
    pub struct Plan {
        raw: ffi::YeptrisPlan,
    }

    impl Drop for Plan {
        fn drop(&mut self) {
            unsafe { ffi::yeptris_plan_free(self.raw) };
        }
    }

    impl Plan {
        /// Compile from a strict-JSON spec document.
        pub fn compile(spec: &str) -> Result<Plan, Error> {
            let mut st = ffi::YEPTRIS_OK;
            let raw = unsafe {
                ffi::yeptris_plan_compile(
                    spec.as_ptr() as *const std::os::raw::c_char,
                    spec.len(),
                    &mut st,
                )
            };
            if raw.is_null() {
                Err(Error::from_status(st))
            } else {
                Ok(Plan { raw })
            }
        }

        pub fn column_count(&self) -> usize {
            unsafe { ffi::yeptris_plan_column_count(self.raw) }
        }
    }

    /// A column's leaf kind.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub enum PlanColumnKind {
        Int,
        Float,
        Str,
        Bool,
    }

    impl PlanColumnKind {
        fn from_raw(k: std::os::raw::c_int) -> Option<PlanColumnKind> {
            match k {
                ffi::YEP_PLAN_INT => Some(PlanColumnKind::Int),
                ffi::YEP_PLAN_FLOAT => Some(PlanColumnKind::Float),
                ffi::YEP_PLAN_STR => Some(PlanColumnKind::Str),
                ffi::YEP_PLAN_BOOL => Some(PlanColumnKind::Bool),
                _ => None,
            }
        }
    }

    /// A string cell: a (ptr, len) view into the walked document.
    /// The borrow lasts as long as the [`PlanColumns`] that produced
    /// it (which borrows the document).
    #[derive(Clone, Copy)]
    pub struct PlanStr<'a> {
        bytes: &'a [u8],
    }

    impl PlanStr<'_> {
        pub fn as_bytes(&self) -> &[u8] {
            self.bytes
        }

        pub fn as_str(&self) -> &str {
            std::str::from_utf8(self.bytes).unwrap_or_default()
        }
    }

    /// The columnar result of one walk. Borrowed from the document
    /// (string cells view into it); freed on drop.
    pub struct PlanColumns<'doc> {
        raw: ffi::YeptrisPlanResult,
        _doc: &'doc Document<'doc>,
    }

    impl Drop for PlanColumns<'_> {
        fn drop(&mut self) {
            unsafe { ffi::yeptris_plan_result_free(self.raw) };
        }
    }

    impl PlanColumns<'_> {
        pub fn rows(&self) -> usize {
            unsafe { ffi::yeptris_plan_result_rows(self.raw) }
        }

        pub fn column_kind(&self, col: usize) -> Result<PlanColumnKind, Error> {
            let k = unsafe { ffi::yeptris_plan_result_kind(self.raw, col) };
            PlanColumnKind::from_raw(k).ok_or(Error::Arg)
        }

        /// INT and BOOL columns (bools read as 0/1).
        pub fn ints(&self, col: usize) -> Result<&[i64], Error> {
            self.column_kind(col)?;
            let p = unsafe { ffi::yeptris_plan_result_ints(self.raw, col) };
            if p.is_null() {
                return Err(Error::Arg);
            }
            Ok(unsafe { std::slice::from_raw_parts(p, self.rows()) })
        }

        pub fn floats(&self, col: usize) -> Result<&[f64], Error> {
            self.column_kind(col)?;
            let p = unsafe { ffi::yeptris_plan_result_floats(self.raw, col) };
            if p.is_null() {
                return Err(Error::Arg);
            }
            Ok(unsafe { std::slice::from_raw_parts(p, self.rows()) })
        }

        /// STR columns: (ptr, len) views into the document.
        pub fn strs(&self, col: usize) -> Result<Vec<PlanStr<'_>>, Error> {
            if self.column_kind(col)? != PlanColumnKind::Str {
                return Err(Error::Arg);
            }
            let p = unsafe { ffi::yeptris_plan_result_strs(self.raw, col) };
            if p.is_null() {
                return Err(Error::Arg);
            }
            let raw = unsafe { std::slice::from_raw_parts(p, self.rows()) };
            Ok(raw
                .iter()
                .map(|s| PlanStr {
                    bytes: unsafe {
                        std::slice::from_raw_parts(
                            s.p as *const u8,
                            s.len,
                        )
                    },
                })
                .collect())
        }

        /// Per-column null markers: 1 = the row's leaf was null or
        /// missing. Length equals `rows()`.
        pub fn nulls(&self, col: usize) -> &[u8] {
            let p = unsafe { ffi::yeptris_plan_result_nulls(self.raw, col) };
            if p.is_null() || self.rows() == 0 {
                return &[];
            }
            unsafe { std::slice::from_raw_parts(p, self.rows()) }
        }
    }

    impl Document<'_> {
        /// Apply a compiled plan to this document: typed columns,
        /// one C pass, no per-node host dispatch. The result borrows
        /// the document.
        pub fn plan_walk(&self, plan: &Plan) -> Result<PlanColumns<'_>, Error> {
            let mut st = ffi::YEPTRIS_OK;
            let raw = unsafe {
                ffi::yeptris_document_plan_walk(self.raw, plan.raw, &mut st)
            };
            if raw.is_null() {
                Err(Error::from_status(st))
            } else {
                Ok(PlanColumns {
                    raw,
                    _doc: self,
                })
            }
        }
    }
}

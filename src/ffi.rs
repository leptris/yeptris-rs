//! Hand-rolled FFI declarations for libyeptris — no bindgen, the
//! signatures mirror src/include/yeptris/*.h in the yeptris engine.
//! Field order is the ABI; opaque handles are pointer-sized.

#![allow(non_camel_case_types, dead_code)]

use std::os::raw::{c_char, c_int, c_void};

pub type YeptrisDocument = *mut core::ffi::c_void;
pub type YeptrisNode = *mut core::ffi::c_void;
pub type YeptrisPlan = *mut core::ffi::c_void;
pub type YeptrisPlanResult = *mut core::ffi::c_void;

/* plan.h column kinds */
pub type YeptrisPlanKind = c_int;
pub const YEP_PLAN_INT: YeptrisPlanKind = 0;
pub const YEP_PLAN_FLOAT: YeptrisPlanKind = 1;
pub const YEP_PLAN_STR: YeptrisPlanKind = 2;
pub const YEP_PLAN_BOOL: YeptrisPlanKind = 3;

/* plan.h: the DOM leg's string column rows — (ptr,len) views into
 * the document; the result borrows the document. */
#[repr(C)]
#[derive(Clone, Copy)]
pub struct YeptrisPlanStr {
    pub p: *const c_char,
    pub len: usize,
}

/* yeptris_emit_options (emit.h): size-versioned — the engine reads
 * each field only when `size` covers it, so `size` is always the
 * full sizeof of THIS mirror (every field declared present). Field
 * order and widths mirror the header exactly; tests/abi.rs pins the
 * 24-byte layout. */
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct YeptrisEmitOptions {
    pub size: u32,
    pub canonical: c_int,
    pub best_width: c_int,
    pub explicit_doc_start: c_int,
    pub best_indent: c_int,
    pub block_fold: c_int,
}

const _: () = assert!(std::mem::size_of::<YeptrisEmitOptions>() == 24);

/* YeptrisStatus (error.h) */
pub type YeptrisStatus = c_int;
pub const YEPTRIS_OK: YeptrisStatus = 0;
pub const YEPTRIS_ERROR_PARSE: YeptrisStatus = 1;
pub const YEPTRIS_ERROR_MEMORY: YeptrisStatus = 2;
pub const YEPTRIS_ERROR_DEPTH: YeptrisStatus = 3;
pub const YEPTRIS_ERROR_ENCODING: YeptrisStatus = 4;
pub const YEPTRIS_ERROR_IO: YeptrisStatus = 5;
pub const YEPTRIS_ERROR_ARG: YeptrisStatus = 6;
pub const YEPTRIS_ERROR_UNSUPPORTED: YeptrisStatus = 7;
pub const YEPTRIS_ERROR_INTERNAL: YeptrisStatus = 8;
pub const YEPTRIS_ERROR_SCHEMA: YeptrisStatus = 9;

/* YeptrisNodeKind (dom.h) */
pub type YeptrisNodeKind = c_int;
pub const YEPTRIS_NODE_SCALAR: YeptrisNodeKind = 0;
pub const YEPTRIS_NODE_SEQUENCE: YeptrisNodeKind = 1;
pub const YEPTRIS_NODE_MAPPING: YeptrisNodeKind = 2;
pub const YEPTRIS_NODE_ALIAS: YeptrisNodeKind = 3;

/* YeptrisScalarStyle (dom.h) */
pub type YeptrisScalarStyle = c_int;
pub const YEPTRIS_STYLE_ANY: YeptrisScalarStyle = 0;
pub const YEPTRIS_STYLE_PLAIN: YeptrisScalarStyle = 1;
pub const YEPTRIS_STYLE_SINGLE_QUOTED: YeptrisScalarStyle = 2;
pub const YEPTRIS_STYLE_DOUBLE_QUOTED: YeptrisScalarStyle = 3;
pub const YEPTRIS_STYLE_LITERAL: YeptrisScalarStyle = 4;
pub const YEPTRIS_STYLE_FOLDED: YeptrisScalarStyle = 5;

/* dom.h: bulk build (12-byte entries, ABI-pinned) */
pub const YEPTRIS_BUILD_SCALAR: u8 = 1;
pub const YEPTRIS_BUILD_SEQ: u8 = 2;
pub const YEPTRIS_BUILD_MAP: u8 = 3;
pub const YEPTRIS_BUILD_END: u8 = 4;
pub const YEPTRIS_BUILD_TAG: u8 = 5;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct YeptrisBuildEntry {
    pub op: u8,
    pub style: u8,
    pub reserved: u16,
    pub off: u32,
    pub len: u32,
}

/* events.h — the pull model (StAX-style). The recorder/iterparse/
 * push models stay undeclared until a face drives them. */
pub type YeptrisPullParser = *mut core::ffi::c_void;

/* YeptrisEventType (events.h), ABI-pinned order */
pub type YeptrisEventType = c_int;
pub const YEPTRIS_EV_STREAM_START: YeptrisEventType = 1;
pub const YEPTRIS_EV_STREAM_END: YeptrisEventType = 2;
pub const YEPTRIS_EV_DOCUMENT_START: YeptrisEventType = 3;
pub const YEPTRIS_EV_DOCUMENT_END: YeptrisEventType = 4;
pub const YEPTRIS_EV_SEQUENCE_START: YeptrisEventType = 5;
pub const YEPTRIS_EV_SEQUENCE_END: YeptrisEventType = 6;
pub const YEPTRIS_EV_MAPPING_START: YeptrisEventType = 7;
pub const YEPTRIS_EV_MAPPING_END: YeptrisEventType = 8;
pub const YEPTRIS_EV_SCALAR: YeptrisEventType = 9;
pub const YEPTRIS_EV_ALIAS: YeptrisEventType = 10;

/* YeptrisEvent (events.h): one event, string fields pointing into
 * the owning model's storage (valid until the next pull call). The C
 * `type` field is `kind` here — `type` is a Rust keyword; field names
 * carry no ABI weight under repr(C). */
#[repr(C)]
#[derive(Clone, Copy)]
pub struct YeptrisEvent {
    pub kind: YeptrisEventType,
    pub style: c_int,
    pub flow: c_int,
    pub explicit_marker: c_int,
    pub implicit: c_int,
    pub value: *const c_char,
    pub value_len: usize,
    pub anchor: *const c_char,
    pub anchor_len: usize,
    pub tag: *const c_char,
    pub tag_len: usize,
    pub line: u32,
    pub col: u32,
    pub end_line: u32,
    pub end_col: u32,
}

extern "C" {
    /* version.h */
    pub fn yeptris_version() -> *const c_char;

    /* parse.h */
    pub fn yeptris_parse(
        buf: *const c_char,
        len: usize,
        status: *mut YeptrisStatus,
    ) -> YeptrisDocument;

    /* error.h */
    pub fn yeptris_last_error(line: *mut u32, col: *mut u32) -> *const c_char;

    /* events.h — the pull model */
    pub fn yeptris_pull_new(buf: *const c_char, len: usize) -> YeptrisPullParser;
    pub fn yeptris_pull_next(pull: YeptrisPullParser) -> *const YeptrisEvent;
    pub fn yeptris_pull_next_batch(
        pull: YeptrisPullParser,
        out: *mut YeptrisEvent,
        max: usize,
    ) -> usize;
    pub fn yeptris_pull_status(
        pull: YeptrisPullParser,
        line: *mut u32,
        col: *mut u32,
    ) -> YeptrisStatus;
    pub fn yeptris_pull_free(pull: YeptrisPullParser);

    /* dom.h */
    pub fn yeptris_document_free(doc: YeptrisDocument);
    pub fn yeptris_document_new() -> YeptrisDocument;
    pub fn yeptris_document_build(
        doc: YeptrisDocument,
        entries: *const YeptrisBuildEntry,
        count: usize,
        blob: *const c_char,
        blob_len: usize,
    ) -> YeptrisStatus;
    pub fn yeptris_document_count(doc: YeptrisDocument) -> usize;
    pub fn yeptris_document_root(doc: YeptrisDocument, index: usize) -> YeptrisNode;
    pub fn yeptris_node_kind(node: YeptrisNode) -> YeptrisNodeKind;
    pub fn yeptris_node_id(node: YeptrisNode) -> u32;
    pub fn yeptris_node_value(node: YeptrisNode, len: *mut usize) -> *const c_char;
    pub fn yeptris_node_style(node: YeptrisNode) -> YeptrisScalarStyle;
    pub fn yeptris_node_tag(node: YeptrisNode, len: *mut usize) -> *const c_char;
    pub fn yeptris_node_anchor(node: YeptrisNode, len: *mut usize) -> *const c_char;
    pub fn yeptris_node_alias_target(node: YeptrisNode) -> YeptrisNode;
    pub fn yeptris_node_seq_count(node: YeptrisNode) -> usize;
    pub fn yeptris_node_seq_at(node: YeptrisNode, index: usize) -> YeptrisNode;
    pub fn yeptris_node_map_count(node: YeptrisNode) -> usize;
    pub fn yeptris_node_map_get(
        node: YeptrisNode,
        key: *const c_char,
        key_len: usize,
    ) -> YeptrisNode;
    pub fn yeptris_node_map_at(
        node: YeptrisNode,
        index: usize,
        key: *mut YeptrisNode,
        value: *mut YeptrisNode,
    ) -> c_int;
    /* dom.h: the bulk children drain — sequence items in order;
     * mapping KEY,VALUE pairs interleaved. One sibling-chain walk;
     * handles come from the document's pool (document_free reclaims
     * them). out==NULL or cap==0 returns the count without filling. */
    pub fn yeptris_node_children(node: YeptrisNode, out: *mut YeptrisNode, cap: usize) -> usize;

    /* json.h */
    pub fn yeptris_parse_json(
        buf: *const c_char,
        len: usize,
        status: *mut YeptrisStatus,
    ) -> YeptrisDocument;
    pub fn yeptris_serialize_json(doc: YeptrisDocument, len: *mut usize) -> *mut c_char;

    /* cbor.h */
    pub fn yeptris_cbor_decode(
        buf: *const c_void,
        len: usize,
        opts: u32,
        status: *mut YeptrisStatus,
    ) -> YeptrisDocument;
    pub fn yeptris_cbor_encode(doc: YeptrisDocument, opts: u32, len: *mut usize) -> *mut c_void;

    /* emit.h */
    pub fn yeptris_serialize(doc: YeptrisDocument, len: *mut usize) -> *mut c_char;
    pub fn yeptris_serialize_ex(
        doc: YeptrisDocument,
        opts: *const YeptrisEmitOptions,
        len: *mut usize,
    ) -> *mut c_char;
    pub fn yeptris_serialize_into(doc: YeptrisDocument, buf: *mut c_char, cap: usize) -> usize;

    /* parse.h — the tolerant per-document stream iterator (#921). The
     * handle is opaque; opts NULL rides the parse defaults (the
     * parse_stream contract). Frames are YeptrisDocuments the caller
     * frees; failures carry the failed document's own line/col via
     * _status and consume an index. */
    pub fn yeptris_stream_iter_new(
        buf: *const c_char,
        len: usize,
        opts: *const c_void,
    ) -> *mut c_void;
    pub fn yeptris_stream_iter_next(
        it: *mut c_void,
        out: *mut YeptrisDocument,
        index: *mut usize,
    ) -> YeptrisStatus;
    pub fn yeptris_stream_iter_status(
        it: *const c_void,
        line: *mut u32,
        col: *mut u32,
    ) -> YeptrisStatus;
    pub fn yeptris_stream_iter_free(it: *mut c_void);

    /* plan.h — the compiled-plan columnar walk */
    pub fn yeptris_plan_compile(
        spec: *const c_char,
        len: usize,
        status: *mut YeptrisStatus,
    ) -> YeptrisPlan;
    pub fn yeptris_plan_free(plan: YeptrisPlan);
    pub fn yeptris_plan_column_count(plan: YeptrisPlan) -> usize;
    pub fn yeptris_document_plan_walk(
        doc: YeptrisDocument,
        plan: YeptrisPlan,
        status: *mut YeptrisStatus,
    ) -> YeptrisPlanResult;
    pub fn yeptris_plan_result_free(r: YeptrisPlanResult);
    pub fn yeptris_plan_result_rows(r: YeptrisPlanResult) -> usize;
    pub fn yeptris_plan_result_kind(r: YeptrisPlanResult, col: usize) -> c_int;
    pub fn yeptris_plan_result_ints(r: YeptrisPlanResult, col: usize) -> *const i64;
    pub fn yeptris_plan_result_floats(r: YeptrisPlanResult, col: usize) -> *const f64;
    pub fn yeptris_plan_result_strs(r: YeptrisPlanResult, col: usize) -> *const YeptrisPlanStr;
    pub fn yeptris_plan_result_nulls(r: YeptrisPlanResult, col: usize) -> *const u8;

    /* memory */
    pub fn yeptris_free(p: *mut core::ffi::c_void);
}

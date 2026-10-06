//! Hand-rolled FFI declarations for libyeptris — no bindgen, the
//! signatures mirror src/include/yeptris/*.h in the yeptris engine.
//! Field order is the ABI; opaque handles are pointer-sized.

#![allow(non_camel_case_types, dead_code)]

use std::os::raw::{c_char, c_int};

pub type YeptrisDocument = *mut core::ffi::c_void;
pub type YeptrisNode = *mut core::ffi::c_void;

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

    /* dom.h */
    pub fn yeptris_document_free(doc: YeptrisDocument);
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

    /* emit.h */
    pub fn yeptris_serialize(doc: YeptrisDocument, len: *mut usize) -> *mut c_char;
    pub fn yeptris_serialize_into(doc: YeptrisDocument, buf: *mut c_char, cap: usize) -> usize;

    /* memory */
    pub fn yeptris_free(p: *mut core::ffi::c_void);
}

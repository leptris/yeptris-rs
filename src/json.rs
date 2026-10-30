//! The engine's JSON face (yeptris-rs#17): the same document model the
//! YAML face wraps, parsed and serialized by `yeptris_parse_json` /
//! `yeptris_serialize_json`. JSON is a subset of YAML 1.2 flow, but
//! these entry points take the JSON-specialized routes (schema events,
//! the compat float contract) end to end.

use std::ffi::c_char;

use serde::de::DeserializeOwned;
use serde::Serialize;

use crate::value::Value;
use crate::{Document, Error, Result};

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
    pub fn serialize_json(&self) -> Result<Vec<u8>> {
        let mut len: usize = 0;
        let raw = unsafe { ffi::yeptris_serialize_json(self.raw, &mut len) };
        if raw.is_null() {
            return Err(Error::Memory);
        }
        let out = unsafe { std::slice::from_raw_parts(raw as *const u8, len) }.to_vec();
        unsafe { ffi::yeptris_free(raw as *mut core::ffi::c_void) };
        Ok(out)
    }

    /// Serialize to canonical JSON as an owned UTF-8 string.
    pub fn serialize_json_string(&self) -> Result<String> {
        let bytes = self.serialize_json()?;
        String::from_utf8(bytes).map_err(|_| Error::Internal)
    }
}

/// Parse JSON text into a [`Value`].
pub fn parse(s: &str) -> Result<Value> {
    let doc = Document::parse_json(s.as_bytes())?;
    Value::from_document(&doc)
}

/// Serialize a [`Value`] to canonical JSON (compact, byte-stable).
pub fn serialize(value: &Value) -> Result<String> {
    let doc = emit::build_document(value)?;
    doc.serialize_json_string()
}

/// Deserialize `T` from JSON text (serde over the engine's JSON face).
pub fn from_str<T: DeserializeOwned>(s: &str) -> std::result::Result<T, crate::DeserError> {
    from_slice(s.as_bytes())
}

/// Byte form of [`from_str`].
pub fn from_slice<T: DeserializeOwned>(s: &[u8]) -> std::result::Result<T, crate::DeserError> {
    let doc = Document::parse_json(s)?;
    let value = Value::from_document(&doc)?;
    crate::de::from_value(&value)
}

/// Serialize `T` to canonical JSON text.
pub fn to_string<T: Serialize + ?Sized>(value: &T) -> Result<String> {
    serialize(&crate::ser::to_value(value).map_err(|_| Error::Arg)?)
}

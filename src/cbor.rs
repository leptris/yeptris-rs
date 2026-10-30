//! The engine's CBOR face (yeptris-rs#17): RFC 8949 items through
//! `yeptris_cbor_decode` / `yeptris_cbor_encode`, over the same
//! document model as the YAML and JSON faces.

use std::ffi::c_void;

use serde::de::DeserializeOwned;
use serde::Serialize;

use crate::value::Value;
use crate::{Document, Error, Result};

use crate::emit;
use crate::ffi;

impl Document<'_> {
    /// Decode CBOR bytes into a document. The input is copied into
    /// the engine's arena, so the returned document may outlive it.
    pub fn parse_cbor(input: &[u8]) -> Result<Document<'static>> {
        let mut status: ffi::YeptrisStatus = ffi::YEPTRIS_OK;
        let raw = unsafe {
            ffi::yeptris_cbor_decode(input.as_ptr() as *const c_void, input.len(), 0, &mut status)
        };
        if raw.is_null() {
            return Err(Error::from_status(status));
        }
        Ok(Document {
            raw,
            _input: core::marker::PhantomData,
        })
    }

    /// Encode the document's first root to CBOR bytes (the engine's
    /// canonical form).
    pub fn serialize_cbor(&self) -> Result<Vec<u8>> {
        let mut len: usize = 0;
        let raw = unsafe { ffi::yeptris_cbor_encode(self.raw, 0, &mut len) };
        if raw.is_null() {
            return Err(Error::Memory);
        }
        let out = unsafe { std::slice::from_raw_parts(raw as *const u8, len) }.to_vec();
        unsafe { ffi::yeptris_free(raw) };
        Ok(out)
    }
}

/// Decode CBOR bytes into a [`Value`].
pub fn decode(input: &[u8]) -> Result<Value> {
    let doc = Document::parse_cbor(input)?;
    Value::from_document(&doc)
}

/// Encode a [`Value`] to canonical CBOR bytes.
pub fn encode(value: &Value) -> Result<Vec<u8>> {
    let doc = emit::build_document(value)?;
    doc.serialize_cbor()
}

/// Deserialize `T` from CBOR bytes (serde over the engine's CBOR face).
pub fn from_slice<T: DeserializeOwned>(input: &[u8]) -> std::result::Result<T, crate::DeserError> {
    let value = decode(input)?;
    crate::de::from_value(&value)
}

/// Serialize `T` to canonical CBOR bytes.
pub fn to_vec<T: Serialize + ?Sized>(value: &T) -> Result<Vec<u8>> {
    encode(&crate::ser::to_value(value).map_err(|_| Error::Arg)?)
}

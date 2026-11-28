//! The raw JSON tape face (yeptris/tape.h): ONE record per token in
//! three parallel columns — kind byte, off, len — no DOM nodes, no
//! [`Value`] tree. The simdjson-class materialization the C/Ruby/
//! Python/JS bindings expose, at binding parity (yeptris-rs#135):
//!
//! ```
//! use yeptris::json::{Tape, record};
//!
//! let src = r#"{"n": 3}"#;
//! let tape = Tape::parse(src).unwrap();
//! let cols = tape.columns().unwrap();
//! assert_eq!(record::DOC, cols.kinds()[0]); // every tape opens with one
//! let s = cols.kinds().iter().position(|&k| k == record::STR).unwrap();
//! let (o, l) = (cols.offs()[s] as usize, cols.lens()[s] as usize);
//! assert_eq!(&src[o..o + l], "n");
//! ```
//!
//! The spans borrow the parsed buffer — the `'input` lifetime IS
//! that contract (same shape as [`crate::Document`]). Number records
//! carry text spans only (parse validates the grammar; conversion is
//! the consumer's move, [`crate::json::parse`] shows the bulk route).

use std::ffi::c_char;
use std::fmt;
use std::marker::PhantomData;
use std::slice;

use crate::ffi;
use crate::{Error, ErrorKind};

/// The tape record kinds (tape.h's `YEP_T_*`, ABI-pinned order):
/// [`Tape::columns`]' [`TapeColumns::kinds`] slice answers these
/// bytes. `NUM` — the lenient route's deferred number record — never
/// rides a strict-route tape, so this face does not name it.
pub mod record {
    use crate::ffi;

    /// Document boundary: every tape opens with one; the records follow.
    pub const DOC: u8 = ffi::YEP_T_DOC;
    pub const NULL: u8 = ffi::YEP_T_NULL;
    /// Bools encode in the kind — the kind IS the value, no span.
    pub const TRUE: u8 = ffi::YEP_T_TRUE;
    pub const FALSE: u8 = ffi::YEP_T_FALSE;
    /// off/len: signed digit span — convert at materialize time.
    pub const INT: u8 = ffi::YEP_T_INT;
    /// off/len: float text span — convert at materialize time.
    pub const FLOAT: u8 = ffi::YEP_T_FLOAT;
    /// off/len: RAW span into the source — escapes stay untouched.
    pub const STR: u8 = ffi::YEP_T_STR;
    /// off: the index of the matching [`record::CLOSE`].
    pub const SEQ_OPEN: u8 = ffi::YEP_T_SEQ_OPEN;
    /// off: the index of the matching [`record::CLOSE`]; keys are the
    /// STR records at even positions inside.
    pub const MAP_OPEN: u8 = ffi::YEP_T_MAP_OPEN;
    /// off: the index of the matching OPEN record.
    pub const CLOSE: u8 = ffi::YEP_T_CLOSE;
}

/// A parsed JSON tape (yeptris/tape.h): one record per token of one
/// strict parse. `Tape::parse` is the only constructor; the engine
/// block is freed on drop.
pub struct Tape<'input> {
    raw: ffi::YeptrisJsonTape,
    _input: PhantomData<&'input [u8]>,
}

impl Drop for Tape<'_> {
    fn drop(&mut self) {
        unsafe { ffi::yeptris_tape_free(&mut self.raw) };
    }
}

impl fmt::Debug for Tape<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Tape({} records)", self.raw.count)
    }
}

impl<'input> Tape<'input> {
    /// Parse strict JSON into the tape: one engine walk, one record
    /// per token, no tree. Scalar roots ride (a single record after
    /// the boundary). Malformed input fails with the engine's message
    /// and position ([`Error::message`]).
    pub fn parse(input: &'input str) -> Result<Tape<'input>, Error> {
        let mut raw = ffi::YeptrisJsonTape::ZEROED;
        let st = unsafe {
            ffi::yeptris_parse_json_tape(input.as_ptr() as *const c_char, input.len(), &mut raw)
        };
        if st != ffi::YEPTRIS_OK {
            return Err(Error::from_status(st));
        }
        Ok(Tape {
            raw,
            _input: PhantomData,
        })
    }

    /// The record count (the DOC boundary included).
    pub fn count(&self) -> usize {
        self.raw.count
    }

    /// The three parallel columns, materialized and handed back as
    /// slices — one entry per record. The strict route builds them at
    /// parse, so [`yeptris_tape_columns`] is a no-op here; the C
    /// contract makes the call mandatory before reading the pointers
    /// (a lenient-route tape's columns are lazy), so this face makes
    /// it unconditionally.
    pub fn columns(&self) -> Result<TapeColumns<'_>, Error> {
        let rc = unsafe {
            ffi::yeptris_tape_columns(
                &self.raw as *const ffi::YeptrisJsonTape as *mut ffi::YeptrisJsonTape,
            )
        };
        if rc != 0 {
            return Err(ErrorKind::Memory.into());
        }
        let n = self.raw.count;
        let (kinds, offs, lens) = unsafe {
            (
                col(self.raw.kinds, n),
                col(self.raw.offs, n),
                col(self.raw.lens, n),
            )
        };
        Ok(TapeColumns { kinds, offs, lens })
    }

    /// The raw handle (crate-internal): the plan face's tape leg
    /// walks this tape in C ([`crate::json::Plan::walk`]).
    pub(crate) fn ptr(&self) -> *const ffi::YeptrisJsonTape {
        &self.raw
    }
}

/// A column pointer as a slice. NULL (which a zero-record tape
/// carries; the record count can never be zero in practice) reads as
/// empty rather than crossing `from_raw_parts` with a null base.
///
/// # Safety
/// `p` must be null or valid for `n` elements of `T` (the engine's
/// carve/columns contract for the tape it produced).
unsafe fn col<'a, T>(p: *mut T, n: usize) -> &'a [T] {
    if p.is_null() || n == 0 {
        return &[];
    }
    unsafe { slice::from_raw_parts(p, n) }
}

/// One column materialization: three parallel slices, one entry per
/// record ([`Tape::columns`]). `kinds` answers the [`record`] bytes;
/// `offs`/`lens` are span starts and lengths into the SOURCE for the
/// str/int/float records and container links (the matching open/
/// close index) for the open/close records.
#[derive(Clone, Copy, Debug)]
pub struct TapeColumns<'a> {
    kinds: &'a [u8],
    offs: &'a [u32],
    lens: &'a [u32],
}

impl<'a> TapeColumns<'a> {
    /// The kind byte per record (see the [`record`] constants).
    pub fn kinds(&self) -> &'a [u8] {
        self.kinds
    }

    /// Span starts: str/int/float records point into the source;
    /// open/close records carry the matching record's index.
    pub fn offs(&self) -> &'a [u32] {
        self.offs
    }

    /// Span lengths (str/int/float records; 0 elsewhere).
    pub fn lens(&self) -> &'a [u32] {
        self.lens
    }

    /// The record count (the DOC boundary included).
    pub fn count(&self) -> usize {
        self.kinds.len()
    }
}

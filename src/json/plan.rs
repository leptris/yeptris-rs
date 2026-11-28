//! The compiled plan's JSON tape leg (yeptris/plan.h): a plan compiled
//! once from a strict-JSON spec, applied to JSON data in ONE C pass
//! (one tape parse + one `yeptris_tape_plan_walk`) producing typed
//! COLUMNAR output — no intermediate [`Value`] tree, no per-row host
//! calls. The economy the C/Ruby/Python/JS bindings expose, at
//! binding parity (yeptris-rs#135):
//!
//! ```
//! use yeptris::json::Plan;
//!
//! let plan = Plan::compile(
//!     r#"{"kind":"map","path":"users","children":[
//!         {"name":"id","kind":"int"},{"name":"name","kind":"str"}]}"#,
//! )
//! .unwrap();
//! let cols = plan.walk(r#"{"users":[{"id":7,"name":"ada"}]}"#).unwrap();
//! assert_eq!(cols.rows(), 1);
//! assert_eq!(cols.ints(0).unwrap(), &[7]);
//! assert_eq!(cols.strs(1).unwrap(), ["ada"]);
//! ```
//!
//! The spec grammar, leaf kinds, and null contract are the C
//! compiler's (plan.h): `kind` is `"seq"` or `"map"` — the ROWS
//! container's shape — `path` selects it (a string, or an array of
//! strings for a segmented path), and every `child` names one typed
//! column. Explicit null and missing leaves mark the slot null; an
//! EMPTY rows container is the C contract's shape disagreement
//! ([`ErrorKind::Parse`]).
//!
//! This leg shares the engine plan handle with [`crate::plan::Plan`]
//! (the DOM leg over a parsed YAML/JSON document). Its string cells
//! are zero-copy spans into the walked data — RAW, escape sequences
//! untouched, the tape face's contract; decode a span through
//! [`crate::json::parse`] when decoded text is wanted.

use std::fmt;
use std::slice;

use crate::ffi;
use crate::json::tape::Tape;
use crate::plan::PlanColumnKind;
use crate::{Error, ErrorKind};

/// A compiled plan over the JSON tape route. The same engine handle
/// as [`crate::plan::Plan`] (the DOM leg); this wrapper owns the
/// tape-leg walk door.
pub struct Plan {
    inner: crate::plan::Plan,
}

impl fmt::Debug for Plan {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "Plan({} columns)", self.column_count())
    }
}

impl Plan {
    /// Compile from a strict-JSON spec document (plan.h's grammar —
    /// the same spec the sibling bindings take). Malformed specs
    /// fail with the engine's message.
    pub fn compile(spec: &str) -> Result<Plan, Error> {
        Ok(Plan {
            inner: crate::plan::Plan::compile(spec)?,
        })
    }

    /// The planned columns' count (the spec's `children` length).
    pub fn column_count(&self) -> usize {
        self.inner.column_count()
    }

    /// Apply the plan to strict-JSON `data`: one tape parse, one C
    /// plan pass, typed columns back. The [`WalkResult`] borrows
    /// `data` (string cells are spans into it), so the walk compiles
    /// to two FFI crossings and the free ordering (result before
    /// source) is the borrow checker's law, not the caller's.
    pub fn walk<'a>(&self, data: &'a str) -> Result<WalkResult<'a>, Error> {
        let tape = Tape::parse(data)?;
        let mut st = ffi::YEPTRIS_OK;
        let raw = unsafe { ffi::yeptris_tape_plan_walk(tape.ptr(), self.inner.raw(), &mut st) };
        // The tape's own block is released here; the result's string
        // cells are (off,len) spans into `data`, which the lifetime
        // pins — the C contract's "free the result before the tape/
        // source" holds with room to spare.
        drop(tape);
        if raw.is_null() {
            return Err(Error::from_status(st));
        }
        Ok(WalkResult { raw, src: data })
    }
}

/// The columnar result of one tape-leg walk ([`Plan::walk`]).
/// Borrowed from the walked data (string cells are spans into it);
/// freed on drop.
pub struct WalkResult<'a> {
    raw: ffi::YeptrisPlanResult,
    src: &'a str,
}

impl Drop for WalkResult<'_> {
    fn drop(&mut self) {
        unsafe { ffi::yeptris_plan_result_free(self.raw) };
    }
}

impl fmt::Debug for WalkResult<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "WalkResult({} rows)", self.rows())
    }
}

impl<'a> WalkResult<'a> {
    /// The number of rows the walk filled.
    pub fn rows(&self) -> usize {
        unsafe { ffi::yeptris_plan_result_rows(self.raw) }
    }

    /// A column's leaf kind ([`ErrorKind::Arg`] past the last one).
    pub fn column_kind(&self, col: usize) -> Result<PlanColumnKind, Error> {
        let k = unsafe { ffi::yeptris_plan_result_kind(self.raw, col) };
        PlanColumnKind::from_raw(k)
            .ok_or(ErrorKind::Arg)
            .map_err(Error::from)
    }

    /// INT and BOOL columns (bools read as 0/1, the engine's
    /// columnar contract).
    pub fn ints(&self, col: usize) -> Result<&[i64], Error> {
        self.column_kind(col)?;
        let p = unsafe { ffi::yeptris_plan_result_ints(self.raw, col) };
        if p.is_null() {
            return Err(ErrorKind::Arg.into());
        }
        Ok(unsafe { slice::from_raw_parts(p, self.rows()) })
    }

    /// FLOAT columns (INT leaves cast at the C leg; plan.h).
    pub fn floats(&self, col: usize) -> Result<&[f64], Error> {
        self.column_kind(col)?;
        let p = unsafe { ffi::yeptris_plan_result_floats(self.raw, col) };
        if p.is_null() {
            return Err(ErrorKind::Arg.into());
        }
        Ok(unsafe { slice::from_raw_parts(p, self.rows()) })
    }

    /// STR columns: RAW spans (escape sequences untouched) into the
    /// walked data. Null rows read as the empty span — the
    /// [`WalkResult::nulls`] bitmap is the truth.
    pub fn strs(&self, col: usize) -> Result<Vec<&'a str>, Error> {
        if self.column_kind(col)? != PlanColumnKind::Str {
            return Err(ErrorKind::Arg.into());
        }
        let offs = unsafe { ffi::yeptris_plan_result_str_offs(self.raw, col) };
        let lens = unsafe { ffi::yeptris_plan_result_str_lens(self.raw, col) };
        if offs.is_null() || lens.is_null() {
            return Err(ErrorKind::Arg.into());
        }
        let (offs, lens) = unsafe {
            (
                slice::from_raw_parts(offs, self.rows()),
                slice::from_raw_parts(lens, self.rows()),
            )
        };
        Ok(offs
            .iter()
            .zip(lens)
            .map(|(&o, &l)| &self.src[o as usize..o as usize + l as usize])
            .collect())
    }

    /// Per-column null markers: 1 = the row's leaf was null or
    /// missing. Length equals [`WalkResult::rows`].
    pub fn nulls(&self, col: usize) -> &[u8] {
        let p = unsafe { ffi::yeptris_plan_result_nulls(self.raw, col) };
        if p.is_null() || self.rows() == 0 {
            return &[];
        }
        unsafe { slice::from_raw_parts(p, self.rows()) }
    }
}

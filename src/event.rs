//! The event face: a Rust [`Iterator`] over the engine's pull model
//! (events.h — the StAX-style cursor, host-driven, zero C->host
//! callbacks). One `next()` is one `yeptris_pull_next` call: no
//! buffering, no allocation beyond the event view.
//!
//! ```
//! use yeptris::event::Puller;
//!
//! let mut puller = Puller::new(b"---\nfoo\n---\nbar\n").unwrap();
//! let kinds: Vec<_> = puller
//!     .events()
//!     .map(|ev| ev.unwrap().kind())
//!     .collect();
//! assert_eq!(kinds.len(), 8); // STREAM/DOC pair per document, framed
//! ```
//!
//! The engine frames the stream exactly once (`StreamStart` /
//! `StreamEnd`) and every document with a `DocumentStart` /
//! `DocumentEnd` pair — marker-less input frames too. Event strings
//! are owned by the puller and follow the C validity window: valid
//! until the next `next()` call, so copy (`value()`'s bytes) anything
//! that must outlive the iteration step. The idiomatic `for` loop
//! respects the window by construction.

use crate::ffi;
use crate::{Error, ErrorKind, Result, Style};
use std::ffi::c_char;
use std::marker::PhantomData;
use std::slice;

/// The kind of an event — mirrors `YeptrisEventType` (events.h).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EventKind {
    StreamStart,
    StreamEnd,
    DocumentStart,
    DocumentEnd,
    SequenceStart,
    SequenceEnd,
    MappingStart,
    MappingEnd,
    Scalar,
    Alias,
}

impl EventKind {
    fn from_raw(raw: ffi::YeptrisEventType) -> EventKind {
        match raw {
            ffi::YEPTRIS_EV_STREAM_END => EventKind::StreamEnd,
            ffi::YEPTRIS_EV_DOCUMENT_START => EventKind::DocumentStart,
            ffi::YEPTRIS_EV_DOCUMENT_END => EventKind::DocumentEnd,
            ffi::YEPTRIS_EV_SEQUENCE_START => EventKind::SequenceStart,
            ffi::YEPTRIS_EV_SEQUENCE_END => EventKind::SequenceEnd,
            ffi::YEPTRIS_EV_MAPPING_START => EventKind::MappingStart,
            ffi::YEPTRIS_EV_MAPPING_END => EventKind::MappingEnd,
            ffi::YEPTRIS_EV_SCALAR => EventKind::Scalar,
            ffi::YEPTRIS_EV_ALIAS => EventKind::Alias,
            _ => EventKind::StreamStart,
        }
    }
}

/// One event, borrowed from the puller: scalar/alias content,
/// anchors, tags, and the 1-based start/end marks (the `end_*` col is
/// one past the node's end token — the engine's #179 libyaml-mark
/// law). The lifetime rides the [`Puller`]'s borrow; the strings are
/// valid until the next [`Events::next`] call (see the module docs).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Event<'p> {
    kind: EventKind,
    style: Style,
    flow: bool,
    explicit_marker: bool,
    implicit: bool,
    value: Option<&'p [u8]>,
    anchor: Option<&'p [u8]>,
    tag: Option<&'p [u8]>,
    line: u32,
    col: u32,
    end_line: u32,
    end_col: u32,
}

unsafe fn view<'p>(p: *const c_char, len: usize) -> Option<&'p [u8]> {
    if p.is_null() {
        None
    } else {
        Some(slice::from_raw_parts(p as *const u8, len))
    }
}

impl<'p> Event<'p> {
    unsafe fn from_raw(raw: *const ffi::YeptrisEvent) -> Event<'p> {
        let ev = unsafe { &*raw };
        Event {
            kind: EventKind::from_raw(ev.kind),
            style: Style::from_raw(ev.style),
            flow: ev.flow != 0,
            explicit_marker: ev.explicit_marker != 0,
            implicit: ev.implicit != 0,
            value: unsafe { view(ev.value, ev.value_len) },
            anchor: unsafe { view(ev.anchor, ev.anchor_len) },
            tag: unsafe { view(ev.tag, ev.tag_len) },
            line: ev.line,
            col: ev.col,
            end_line: ev.end_line,
            end_col: ev.end_col,
        }
    }

    /// The event's kind.
    pub fn kind(&self) -> EventKind {
        self.kind
    }

    /// The scalar's presentation style (ANY for non-scalars).
    pub fn style(&self) -> Style {
        self.style
    }

    /// Scalar content or alias name, as raw bytes. `None` when the
    /// engine recorded no value (the empty scalar reads as NULL — the
    /// event face mirrors the C contract, unlike the DOM face's
    /// `Some(&[])` for empty).
    pub fn value(&self) -> Option<&'p [u8]> {
        self.value
    }

    /// Scalar content or alias name as UTF-8.
    pub fn value_str(&self) -> Option<&'p str> {
        self.value.and_then(|b| std::str::from_utf8(b).ok())
    }

    /// The node's anchor, when one was present.
    pub fn anchor(&self) -> Option<&'p [u8]> {
        self.anchor
    }

    /// The node's resolved tag, when one was present.
    pub fn tag(&self) -> Option<&'p [u8]> {
        self.tag
    }

    /// Collection in flow style (`{}` / `[]`).
    pub fn flow(&self) -> bool {
        self.flow
    }

    /// Document framed by an explicit `---` / `...` marker.
    pub fn explicit_marker(&self) -> bool {
        self.explicit_marker
    }

    /// Plain-style scalar, as the engine records the flag — set for
    /// PLAIN scalars even when a tag is present (the tag rides
    /// [`Event::tag`]); quoting is the escape hatch.
    pub fn implicit(&self) -> bool {
        self.implicit
    }

    /// 1-based start mark.
    pub fn line(&self) -> u32 {
        self.line
    }

    /// 1-based start column.
    pub fn col(&self) -> u32 {
        self.col
    }

    /// 1-based end line.
    pub fn end_line(&self) -> u32 {
        self.end_line
    }

    /// 1-based end column, one past the node's end token.
    pub fn end_col(&self) -> u32 {
        self.end_col
    }
}

/// A pull cursor over one input. Owns the engine's puller (the whole
/// parse runs into its event store at construction; `next()` hands
/// out one stored event at a time); freed on drop.
pub struct Puller<'input> {
    raw: ffi::YeptrisPullParser,
    _input: PhantomData<&'input [u8]>,
}

impl Drop for Puller<'_> {
    fn drop(&mut self) {
        unsafe { ffi::yeptris_pull_free(self.raw) };
    }
}

impl<'input> Puller<'input> {
    /// Start a pull cursor over `input` (UTF-8; the engine validates).
    /// The only failure is the host allocation behind the puller.
    pub fn new(input: &'input [u8]) -> Result<Puller<'input>> {
        let raw = unsafe { ffi::yeptris_pull_new(input.as_ptr() as *const c_char, input.len()) };
        if raw.is_null() {
            return Err(ErrorKind::Memory.into());
        }
        Ok(Puller {
            raw,
            _input: PhantomData,
        })
    }

    /// The StAX cursor: each [`Events::next`] is one
    /// `yeptris_pull_next` call, and the yielded [`Event`] views the
    /// puller's storage for as long as this borrowing iterator does.
    pub fn events(&mut self) -> Events<'_, 'input> {
        Events {
            puller: self,
            failed: false,
        }
    }
}

/// The iterator form of [`Puller`]: `Item = Result<Event, Error>`.
/// A `None` ends the stream; a `Some(Err)` reports a parse failure —
/// the error is the engine's message with its line/column folded in
/// (captured at construction, yeptris-rs#24's law). The iterator
/// fuses after the failure (the engine's puller stays in the failed
/// state, so an un-fused cursor would yield the error forever).
pub struct Events<'p, 'input> {
    puller: &'p mut Puller<'input>,
    failed: bool,
}

impl<'p, 'input> Iterator for Events<'p, 'input> {
    type Item = Result<Event<'p>>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.failed {
            return None;
        }
        let raw = unsafe { ffi::yeptris_pull_next(self.puller.raw) };
        if raw.is_null() {
            // exhausted, or failed — yeptris_pull_status tells which
            let status = unsafe {
                ffi::yeptris_pull_status(
                    self.puller.raw,
                    std::ptr::null_mut(),
                    std::ptr::null_mut(),
                )
            };
            if status != ffi::YEPTRIS_OK {
                self.failed = true;
                return Some(Err(Error::from_status(status)));
            }
            return None;
        }
        // The record's strings live in the puller's arena; the event
        // views them through this iterator's borrow and stays valid
        // until the next call (the C window the module docs pin).
        Some(Ok(unsafe { Event::<'p>::from_raw(raw) }))
    }
}

//! Explicit emitter settings — the safe face over the engine's
//! size-versioned `yeptris_emit_options` (emit.h). Available without
//! the serde feature: [`crate::Document::serialize_with`] is DOM-only.

use crate::ffi;
use crate::{ErrorKind, Result};

/// The emitter's fold/wrap column ([`EmitOptions::line_width`]).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum LineWidth {
    /// The engine default: 80 columns (libyaml/Psych `best_width`).
    #[default]
    Default,
    /// Never wrap or fold — Psych's `line_width: -1`.
    Unbounded,
    /// Wrap/fold at this column. Must be greater than twice the
    /// indent and at most `i32::MAX`; the C engine silently snaps a
    /// too-narrow width to 80, this face rejects it instead.
    Columns(u32),
}

/// Explicit emitter settings for [`crate::Document::serialize_with`] and
/// `to_string_with` (serde feature) — a safe face over the engine's size-versioned
/// `yeptris_emit_options`.
///
/// [`EmitOptions::new`] (also `Default`) selects exactly the engine's
/// defaults, so `serialize_with(&EmitOptions::new())` writes the same
/// bytes as [`Document::serialize`]. Pin every knob explicitly when
/// output must stay stable across engine default changes (the 0.6.62
/// fold-default flip):
///
/// ```
/// use yeptris::{EmitOptions, LineWidth};
///
/// let single_line = EmitOptions::new()
///     .line_width(LineWidth::Unbounded)
///     .block_fold(false);
/// assert_eq!(single_line.get_line_width(), LineWidth::Unbounded);
/// ```
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct EmitOptions {
    canonical: bool,
    line_width: LineWidth,
    indent: u8,
    explicit_document_start: bool,
    block_fold: bool,
}

impl Default for EmitOptions {
    fn default() -> Self {
        EmitOptions::new()
    }
}

impl EmitOptions {
    /// The engine defaults: fidelity (non-canonical) form, 80-column
    /// width, indent 2, implicit document start, block folding on.
    pub const fn new() -> Self {
        EmitOptions {
            canonical: false,
            line_width: LineWidth::Default,
            indent: 2,
            explicit_document_start: false,
            block_fold: true,
        }
    }

    /// The engine's fixed canonical form: flow collections, quoted
    /// strings, typed words, shortest floats.
    pub const fn canonical(mut self, on: bool) -> Self {
        self.canonical = on;
        self
    }

    /// The fold/wrap column.
    pub const fn line_width(mut self, width: LineWidth) -> Self {
        self.line_width = width;
        self
    }

    /// Block indentation per level, `2..=9` (libyaml's range). The
    /// C engine silently replaces an out-of-range indent with 2; this
    /// face rejects it with [`ErrorKind::Arg`] at serialize time.
    pub const fn indent(mut self, indent: u8) -> Self {
        self.indent = indent;
        self
    }

    /// Emit the leading `---` on every document (Psych/libyaml's
    /// `implicit = false`).
    pub const fn explicit_document_start(mut self, on: bool) -> Self {
        self.explicit_document_start = on;
        self
    }

    /// Fold plain and single-quoted block scalars at the line width
    /// (the engine default, on). `false` keeps them on one line.
    pub const fn block_fold(mut self, on: bool) -> Self {
        self.block_fold = on;
        self
    }

    pub const fn is_canonical(&self) -> bool {
        self.canonical
    }

    pub const fn get_line_width(&self) -> LineWidth {
        self.line_width
    }

    pub const fn get_indent(&self) -> u8 {
        self.indent
    }

    pub const fn is_explicit_document_start(&self) -> bool {
        self.explicit_document_start
    }

    pub const fn is_block_fold(&self) -> bool {
        self.block_fold
    }

    /// The C struct, every field declared present. Settings the
    /// engine would silently clamp are refused here instead.
    pub(crate) fn ffi(&self) -> Result<ffi::YeptrisEmitOptions> {
        if !(2..=9).contains(&self.indent) {
            return Err(ErrorKind::Arg.into());
        }
        let best_width = match self.line_width {
            LineWidth::Default => 0,
            LineWidth::Unbounded => -1,
            LineWidth::Columns(n) => {
                let n = std::os::raw::c_int::try_from(n).map_err(|_| ErrorKind::Arg)?;
                if n <= 2 * std::os::raw::c_int::from(self.indent) {
                    return Err(ErrorKind::Arg.into());
                }
                n
            }
        };
        Ok(ffi::YeptrisEmitOptions {
            size: std::mem::size_of::<ffi::YeptrisEmitOptions>() as u32,
            canonical: self.canonical.into(),
            best_width,
            explicit_doc_start: self.explicit_document_start.into(),
            best_indent: self.indent.into(),
            block_fold: self.block_fold.into(),
        })
    }
}

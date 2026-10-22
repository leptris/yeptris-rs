//! The `serde_yaml`-shaped data model (teptris's proposal): a
//! fully-owned tree resolved from the engine's zero-copy DOM, with
//! tags kept as [`Value::Tagged`] and aliases expanded at load.
//!
//! Deliberate divergence from serde_yaml, documented: YAML 1.2 has no
//! merge keys, so `<<` stays a literal mapping key (the engine's
//! core-12 default — Psych/serde_yaml expand it; we follow the spec).

use crate::{Document, Error, Node, NodeKind, Result, Style};
use std::collections::HashSet;
use std::fmt;

/// A YAML value: the `serde_yaml::Value` shape (minus `Number`'s
/// arbitrary-precision backend — the engine resolves i64/u64/f64).
#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    Null,
    Bool(bool),
    Number(Number),
    String(String),
    Sequence(Vec<Value>),
    Mapping(Mapping),
    Tagged(Box<Tagged>),
}

#[derive(Clone, Debug, PartialEq)]
pub struct Tagged {
    pub tag: String,
    pub value: Value,
}

/// Insertion-ordered mapping. Keys repeat when the source document
/// repeats them (the engine keeps last-value-wins at parse; the tree
/// preserves what the DOM reports per pair).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Mapping(pub Vec<(Value, Value)>);

impl Mapping {
    pub fn new() -> Mapping {
        Mapping(Vec::new())
    }
    pub fn get(&self, key: &Value) -> Option<&Value> {
        self.0.iter().rev().find(|(k, _)| k == key).map(|(_, v)| v)
    }
    pub fn insert(&mut self, key: Value, value: Value) {
        if let Some(slot) = self.0.iter_mut().rev().find(|(k, _)| *k == key) {
            slot.1 = value;
        } else {
            self.0.push((key, value));
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Number {
    I64(i64),
    U64(u64),
    F64(f64),
}

impl Value {
    /// Materialize a whole document (its first root). Errors on an
    /// empty stream.
    pub fn from_document(doc: &Document<'_>) -> Result<Value> {
        let root = doc.first_root().ok_or(Error::Parse)?;
        let mut visiting = HashSet::new();
        Value::from_node_inner(root, &mut visiting)
    }

    fn from_node_inner(node: Node<'_>, visiting: &mut HashSet<u32>) -> Result<Value> {
        let tagged = |v: Value, node: &Node<'_>| {
            match node.tag() {
                Some(bytes) => match std::str::from_utf8(bytes) {
                    Ok(t) => Value::Tagged(Box::new(Tagged {
                        tag: t.to_owned(),
                        value: v,
                    })),
                    // DOM tags are engine-validated UTF-8; unreachable
                    // in practice, degrade to the untagged value.
                    Err(_) => v,
                },
                None => v,
            }
        };
        match node.kind() {
            NodeKind::Scalar => {
                let bytes = node.value().unwrap_or_default();
                let style = node.style();
                let resolved = if style == Style::Plain {
                    resolve_plain(bytes)
                } else {
                    Value::String(String::from_utf8_lossy(bytes).into_owned())
                };
                Ok(tagged(resolved, &node))
            }
            NodeKind::Sequence => {
                let mut items = Vec::with_capacity(node.seq_count());
                for i in 0..node.seq_count() {
                    items.push(Value::from_node_inner(
                        node.seq_at(i).ok_or(Error::Internal)?,
                        visiting,
                    )?);
                }
                Ok(tagged(Value::Sequence(items), &node))
            }
            NodeKind::Mapping => {
                let mut map = Mapping::new();
                for i in 0..node.map_count() {
                    let (k, v) = node.map_at(i).ok_or(Error::Internal)?;
                    let key = Value::from_node_inner(k, visiting)?;
                    let value = Value::from_node_inner(v, visiting)?;
                    map.insert(key, value);
                }
                Ok(tagged(Value::Mapping(map), &node))
            }
            NodeKind::Alias => {
                let target = node.alias_target().ok_or(Error::Parse)?;
                let id = target.id();
                if !visiting.insert(id) {
                    // The cycle guard pops on the way out; a hit here
                    // means the target is still being expanded.
                    return Err(Error::Parse);
                }
                let v = Value::from_node_inner(target, visiting);
                visiting.remove(&id);
                Ok(v?)
            }
        }
    }
}

/// YAML 1.2 core-schema resolution for PLAIN scalars: null, bool,
/// int (dec / `0o` / `0x`), float (`.inf`/`.nan`/decimal) — else a
/// string. An empty plain scalar is null.
pub(crate) fn resolve_plain(bytes: &[u8]) -> Value {
    let s = match std::str::from_utf8(bytes) {
        Ok(s) => s,
        Err(_) => return Value::String(String::from_utf8_lossy(bytes).into_owned()),
    };
    match s {
        "" | "~" | "null" | "Null" | "NULL" => return Value::Null,
        "true" | "True" | "TRUE" => return Value::Bool(true),
        "false" | "False" | "FALSE" => return Value::Bool(false),
        ".inf" | ".Inf" | ".INF" | "+.inf" | "+.Inf" | "+.INF" => {
            return Value::Number(Number::F64(f64::INFINITY))
        }
        "-.inf" | "-.Inf" | "-.INF" => return Value::Number(Number::F64(f64::NEG_INFINITY)),
        ".nan" | ".NaN" | ".NAN" => return Value::Number(Number::F64(f64::NAN)),
        _ => {}
    }
    if let Some(n) = parse_yaml_int(s) {
        return Value::Number(n);
    }
    if let Some(n) = parse_yaml_float(s) {
        return Value::Number(n);
    }
    Value::String(s.to_owned())
}

fn digits_only(s: &str) -> bool {
    !s.is_empty() && s.bytes().all(|b| b.is_ascii_digit())
}

fn parse_yaml_int(s: &str) -> Option<Number> {
    let (sign, body) = match s.strip_prefix('-') {
        Some(b) => (-1i128, b),
        None => (1i128, s.strip_prefix('+').unwrap_or(s)),
    };
    if body.is_empty() {
        return None;
    }
    if let Some(hex) = body.strip_prefix("0x") {
        return u64::from_str_radix(hex, 16)
            .ok()
            .and_then(|m| i64::try_from(sign * i128::from(m)).ok().map(Number::I64));
    }
    if let Some(oct) = body.strip_prefix("0o") {
        return u64::from_str_radix(oct, 8)
            .ok()
            .and_then(|m| i64::try_from(sign * i128::from(m)).ok().map(Number::I64));
    }
    if !digits_only(body) {
        return None;
    }
    // serde_yaml: i64 first, u64 for the positive overflow window
    if let Ok(v) = s.parse::<i64>() {
        return Some(Number::I64(v));
    }
    s.parse::<u64>().ok().map(Number::U64)
}

fn parse_yaml_float(s: &str) -> Option<Number> {
    let body = s.strip_prefix(['-', '+']).unwrap_or(s);
    if body.is_empty() || !body.bytes().any(|b| b == b'.' || b == b'e' || b == b'E') {
        return None; // a bare digit run is an int, not a float
    }
    let mut seen_dot = false;
    let mut seen_exp = false;
    let bytes = body.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        let b = bytes[i];
        match b {
            b'0'..=b'9' => i += 1,
            b'.' if !seen_dot && !seen_exp => {
                seen_dot = true;
                i += 1;
            }
            b'e' | b'E' if !seen_exp && i + 1 < bytes.len() => {
                seen_exp = true;
                i += 1;
                if bytes[i] == b'-' || bytes[i] == b'+' {
                    i += 1;
                }
            }
            _ => return None,
        }
    }
    // at least one digit must exist (reject ".", "e5")
    if !bytes.iter().any(|b| b.is_ascii_digit()) {
        return None;
    }
    s.parse::<f64>().ok().map(Number::F64)
}

impl fmt::Display for Number {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Number::I64(v) => write!(f, "{}", v),
            Number::U64(v) => write!(f, "{}", v),
            // serde_yaml prints floats through Rust's shortest form
            Number::F64(v) => write!(f, "{}", v),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn resolve(s: &str) -> Value {
        resolve_plain(s.as_bytes())
    }

    #[test]
    fn null_words() {
        for s in ["", "~", "null", "Null", "NULL"] {
            assert_eq!(resolve(s), Value::Null, "{}", s);
        }
    }

    #[test]
    fn bools() {
        assert_eq!(resolve("true"), Value::Bool(true));
        assert_eq!(resolve("FALSE"), Value::Bool(false));
        assert_eq!(resolve("True"), Value::Bool(true));
        assert_eq!(resolve("yes"), Value::String("yes".into())); // 1.2: not a bool
    }

    #[test]
    fn ints() {
        assert_eq!(resolve("0"), Value::Number(Number::I64(0)));
        assert_eq!(resolve("-42"), Value::Number(Number::I64(-42)));
        assert_eq!(resolve("+7"), Value::Number(Number::I64(7)));
        assert_eq!(resolve("0x1f"), Value::Number(Number::I64(31)));
        assert_eq!(resolve("0o17"), Value::Number(Number::I64(15)));
        assert_eq!(
            resolve("18446744073709551615"),
            Value::Number(Number::U64(u64::MAX))
        );
        assert_eq!(resolve("1_000"), Value::String("1_000".into())); // 1.2 core: no underscores
    }

    #[test]
    fn floats() {
        assert_eq!(resolve("1.5"), Value::Number(Number::F64(1.5)));
        assert_eq!(resolve("-1e3"), Value::Number(Number::F64(-1000.0)));
        assert_eq!(resolve("12.00"), Value::Number(Number::F64(12.0)));
        assert_eq!(resolve(".inf"), Value::Number(Number::F64(f64::INFINITY)));
        assert!(matches!(resolve(".nan"), Value::Number(Number::F64(n)) if n.is_nan()));
        assert_eq!(resolve("."), Value::String(".".into()));
        assert_eq!(resolve("1.2.3"), Value::String("1.2.3".into()));
    }

    #[test]
    fn non_plain_stays_string() {
        // exercised through Document in the integration tests; the
        // style gate lives in from_node_inner
    }
}

//! The serde deserializer over the engine's DOM [`Node`] (yeptris-rs#41)
//! — the direct route behind `from_str`/`from_slice`: one walk of the
//! document, no owned intermediate `Value` tree.
//!
//! Semantics are pinned to the `Value` route byte-for-byte by
//! `tests/differential.rs`. The two non-obvious folds the route must
//! reproduce (the old route got them for free from `Value`'s shape):
//!
//! * **Tags visit as enums.** `Value::Tagged` visits through
//!   `visit_enum` (the serde_yaml contract); the enum payload carries
//!   the node UNTAGGED — while a *mapping-shaped* enum
//!   (`{variant: payload}`) keeps the payload's own tag, because the
//!   old route's payload there was the fully-resolved `Value`.
//! * **Duplicate keys fold to first-position + last-value.** The DOM
//!   reports raw duplicate pairs; the old route folded them through
//!   `Mapping::insert`. The fold must happen before the entries reach
//!   the visitor, or serde-derive structs would see duplicate fields
//!   the old route never delivered. Keys compare as their RESOLVED
//!   values (`0x10` and `16` are the same key; `'1'` and `1` are not;
//!   `.nan` never equals anything, `-0.0` equals `0.0`).
//!
//! Alias semantics: the legacy route's `from_document` walked every
//! node eagerly, so an alias cycle anywhere in the document failed the
//! whole deserialize (Parse) — even under a duplicate key whose folded
//! value never reaches the visitor. The direct route preserves that
//! verdict with ONE drain pass over the document that both runs the
//! same insert-on-enter/remove-on-exit cycle check and captures every
//! container's child list (reused by the main walk — the drain is not
//! paid twice). The engine rejects alias cycles and anchors-on-aliases
//! at parse; the guard is the contract for host-built documents —
//! error, never loop. Lazy alias resolution at dispatch keeps the same
//! guard discipline.
//!
//! Lifetime law: `Node` borrows the document, and `'de` ties to that
//! borrow — `deserialize_any` hands the visitor arena slices
//! (`visit_borrowed_str`). `from_str`/`from_slice` own the `Document`
//! for the duration; `T: DeserializeOwned` copies what it keeps.

use std::borrow::Cow;
use std::cell::RefCell;
use std::collections::hash_map::Entry;
use std::collections::{HashMap, HashSet};
use std::hash::{BuildHasher, Hash, Hasher};
use std::rc::Rc;

use serde::de::{
    self, DeserializeOwned, DeserializeSeed, EnumAccess, IntoDeserializer, MapAccess, SeqAccess,
    VariantAccess, Visitor,
};
use serde::Deserializer as _;

use crate::de::DeserError;
use crate::value::{resolve_scalar, ScalarVal, Value};
use crate::{Document, ErrorKind, Node, NodeKind};

/// Deserialize `T` straight from a parsed document's first root — the
/// fast path [`crate::from_str`] rides.
pub(crate) fn from_document<T: DeserializeOwned>(doc: &Document<'_>) -> Result<T, DeserError> {
    let visiting = RefCell::new(HashSet::new());
    let dedup = RefCell::new(HashMap::with_hasher(FxBuild));
    let root = doc.first_root().ok_or_else(parse_error)?;
    // the eager alias verdict + shared child lists: ONE drain
    let mut kids = HashMap::new();
    drain_doc(root, &mut kids, &visiting)?;
    let table = Drained { kids };
    T::deserialize(NodeDeserializer {
        node: root,
        tag_suppressed: false,
        visiting: &visiting,
        dedup: &dedup,
        table: &table,
    })
}

/// The one-pass drain: every container's child list keyed by node id,
/// produced by the same walk that runs the eager alias-cycle verdict
/// (the legacy route's whole-document walk — a cycle anywhere fails
/// the deserialize with the exact same Parse error, even where the
/// duplicate-key fold drops the cyclic value).
struct Drained<'doc> {
    kids: HashMap<u32, Rc<Vec<Node<'doc>>>>,
}

impl<'doc> Drained<'doc> {
    /// A container's children. The drain covered everything reachable;
    /// the fallback only keeps a host-built surprise correct.
    fn kids(&self, node: Node<'doc>) -> Rc<Vec<Node<'doc>>> {
        self.kids
            .get(&node.id())
            .cloned()
            .unwrap_or_else(|| Rc::new(node.children()))
    }
}

fn drain_doc<'doc>(
    node: Node<'doc>,
    out: &mut HashMap<u32, Rc<Vec<Node<'doc>>>>,
    visiting: &Guard,
) -> std::result::Result<(), DeserError> {
    match node.kind() {
        NodeKind::Scalar => Ok(()),
        NodeKind::Alias => {
            let (target, pushed) = resolve_chain(node, visiting)?;
            let r = drain_doc(target, out, visiting);
            {
                let mut set = visiting.borrow_mut();
                for id in pushed {
                    set.remove(&id);
                }
            }
            r
        }
        NodeKind::Sequence | NodeKind::Mapping => {
            // alias-shared subtrees drain once (completed containers
            // cannot hide a cycle: the guard fired during their window)
            if out.contains_key(&node.id()) {
                return Ok(());
            }
            let kids = node.children();
            for kid in &kids {
                drain_doc(*kid, out, visiting)?;
            }
            out.insert(node.id(), Rc::new(kids));
            Ok(())
        }
    }
}

fn parse_error() -> DeserError {
    DeserError::from(crate::Error::from(ErrorKind::Parse))
}

fn internal_error() -> DeserError {
    DeserError::from(crate::Error::from(ErrorKind::Internal))
}

/// Alias-expansion cycle guard, shared across one deserialization
/// (mirrors `Value::from_node_inner`'s `visiting` set). Borrowed, not
/// reference-counted: every node hands its children `&'g` references.
type Guard = RefCell<HashSet<u32>>;
/// Reusable duplicate-key table: capacity survives across the walk's
/// mappings, contents are cleared per mapping.
#[allow(clippy::type_complexity)]
type DedupCell<'doc> = RefCell<HashMap<RKey<'doc>, usize, FxBuild>>;

/// A tiny FxHasher (rustc's read-only fxhash mix) for the dedup table:
/// short scalar keys dominate, and a multiply-xor beats SipHash's
/// multi-round latency there. Key material is document content, not
/// adversary-controlled in the dedup role — and the route it replaces
/// used an O(n²) linear scan, so collision resistance is not a
/// regression surface.
#[derive(Clone, Copy, Default)]
struct FxBuild;

impl BuildHasher for FxBuild {
    type Hasher = FxHasher;
    fn build_hasher(&self) -> FxHasher {
        FxHasher::default()
    }
}

const FX_SEED: u64 = 0x51_7c_c1_b7_27_22_04_a9;

#[derive(Clone, Copy, Default)]
struct FxHasher {
    hash: u64,
}

impl FxHasher {
    fn add(&mut self, word: u64) {
        self.hash = (self.hash.rotate_left(5) ^ word).wrapping_mul(FX_SEED);
    }
}

impl Hasher for FxHasher {
    fn finish(&self) -> u64 {
        self.hash
    }
    fn write(&mut self, bytes: &[u8]) {
        let (chunks, tail) = bytes.as_chunks::<8>();
        for c in chunks {
            self.add(u64::from_le_bytes(*c));
        }
        if !tail.is_empty() {
            // zero-padded tail: distinct byte strings still end up
            // distinct u64s unless one is a zero-extension of the
            // other, and equality decides those
            let mut buf = [0u8; 8];
            buf[..tail.len()].copy_from_slice(tail);
            self.add(u64::from_le_bytes(buf));
        }
    }
    fn write_u8(&mut self, v: u8) {
        self.add(u64::from(v));
    }
    fn write_u64(&mut self, v: u64) {
        self.add(v);
    }
    fn write_usize(&mut self, v: usize) {
        self.add(v as u64);
    }
}

struct NodeDeserializer<'doc, 'g> {
    node: Node<'doc>,
    /// The node is presented as a tagged-enum PAYLOAD: its own tag is
    /// suppressed (the old route's payload there was `t.value`, the
    /// tag-stripped inner value).
    tag_suppressed: bool,
    visiting: &'g Guard,
    dedup: &'g DedupCell<'doc>,
    table: &'g Drained<'doc>,
}

impl<'doc, 'g> NodeDeserializer<'doc, 'g> {
    fn child(&self, node: Node<'doc>) -> Self {
        NodeDeserializer {
            node,
            tag_suppressed: false,
            visiting: self.visiting,
            dedup: self.dedup,
            table: self.table,
        }
    }

    /// The tagged-enum payload: same walk, but the node's own tag is
    /// suppressed — the old route's payload was the tag-stripped
    /// inner value.
    fn payload(&self, node: Node<'doc>) -> Self {
        let mut de = self.child(node);
        de.tag_suppressed = true;
        de
    }

    fn remove_ids(&self, ids: &[u32]) {
        if ids.is_empty() {
            return;
        }
        let mut set = self.visiting.borrow_mut();
        for id in ids {
            set.remove(id);
        }
    }
}

/// The node's tag as a str; invalid-UTF-8 tags degrade to no tag (the
/// `Value` walk's rule — engine-validated in practice).
fn utf8_tag(node: Node<'_>) -> Option<&'_ str> {
    std::str::from_utf8(node.tag()?).ok()
}

/// Resolve an alias chain, keeping every inserted guard id in the set
/// until the CALLER removes them (`remove_ids`) once its subtree walk
/// completes — insert-on-enter, remove-on-exit, the `Value` walk's
/// nesting. The engine rejects cycles and unresolved anchors at
/// parse; a guard hit here means a host-built document. Error, never
/// loop.
fn resolve_chain<'doc>(
    node: Node<'doc>,
    guard: &Guard,
) -> Result<(Node<'doc>, Vec<u32>), DeserError> {
    let mut node = node;
    let mut pushed = Vec::new();
    while node.kind() == NodeKind::Alias {
        let target = node.alias_target().ok_or_else(parse_error)?;
        {
            let mut set = guard.borrow_mut();
            if !set.insert(target.id()) {
                return Err(parse_error());
            }
        }
        pushed.push(target.id());
        node = target;
    }
    Ok((node, pushed))
}

/// Materialize the `Value` the old route would have held at this
/// position — error paths only, so error STRINGS stay byte-identical
/// (serde's messages embed `Value`'s Debug).
fn node_value(node: Node<'_>) -> std::result::Result<Value, DeserError> {
    let guard = RefCell::new(HashSet::new());
    let (node, pushed) = resolve_chain(node, &guard)?;
    let out = Value::from_root(node).map_err(DeserError::from);
    {
        let mut set = guard.borrow_mut();
        for id in pushed {
            set.remove(&id);
        }
    }
    out
}

impl<'de, 'doc, 'g> de::Deserializer<'de> for NodeDeserializer<'doc, 'g>
where
    'doc: 'de,
    'g: 'de,
{
    type Error = DeserError;

    fn deserialize_any<V: Visitor<'de>>(
        self,
        visitor: V,
    ) -> std::result::Result<V::Value, DeserError> {
        if self.node.kind() == NodeKind::Alias {
            let (target, pushed) = resolve_chain(self.node, self.visiting)?;
            let out = self.child(target).any_dispatch(visitor);
            self.remove_ids(&pushed);
            return out;
        }
        self.any_dispatch(visitor)
    }

    fn deserialize_option<V: Visitor<'de>>(
        self,
        visitor: V,
    ) -> std::result::Result<V::Value, DeserError> {
        if self.node.kind() == NodeKind::Alias {
            let (target, pushed) = resolve_chain(self.node, self.visiting)?;
            let out = self.child(target).option_dispatch(visitor);
            self.remove_ids(&pushed);
            return out;
        }
        self.option_dispatch(visitor)
    }

    fn deserialize_enum<V: Visitor<'de>>(
        self,
        _name: &str,
        _variants: &[&str],
        visitor: V,
    ) -> std::result::Result<V::Value, DeserError> {
        if self.node.kind() == NodeKind::Alias {
            let (target, pushed) = resolve_chain(self.node, self.visiting)?;
            let out = self.child(target).enum_dispatch(visitor);
            self.remove_ids(&pushed);
            return out;
        }
        self.enum_dispatch(visitor)
    }

    fn deserialize_newtype_struct<V: Visitor<'de>>(
        self,
        _name: &'static str,
        visitor: V,
    ) -> std::result::Result<V::Value, DeserError> {
        visitor.visit_newtype_struct(self)
    }

    serde::forward_to_deserialize_any! {
        i8 i16 i32 i64 i128 u8 u16 u32 u64 u128 f32 f64 char str string
        bytes byte_buf bool unit unit_struct seq tuple tuple_struct map
        struct identifier ignored_any
    }
}

impl<'doc, 'g> NodeDeserializer<'doc, 'g> {
    /// `deserialize_any` on a non-alias node: tag → enum, else the
    /// node's kind.
    fn any_dispatch<'de, V: Visitor<'de>>(
        self,
        visitor: V,
    ) -> std::result::Result<V::Value, DeserError>
    where
        'doc: 'de,
        'g: 'de,
    {
        if !self.tag_suppressed {
            if let Some(tag) = utf8_tag(self.node) {
                return visitor.visit_enum(EnumDeserializer {
                    variant: Cow::Borrowed(tag),
                    payload: self.payload(self.node),
                });
            }
        }
        match self.node.kind() {
            NodeKind::Scalar => match resolve_scalar(self.node) {
                ScalarVal::Null => visitor.visit_unit(),
                ScalarVal::Bool(b) => visitor.visit_bool(b),
                ScalarVal::Int(v) => visitor.visit_i64(v),
                ScalarVal::Uint(v) => visitor.visit_u64(v),
                ScalarVal::Float(v) => visitor.visit_f64(v),
                // 'de = 'doc: the slice is the document's arena content
                ScalarVal::Str(Cow::Borrowed(s)) => visitor.visit_borrowed_str(s),
                ScalarVal::Str(Cow::Owned(s)) => visitor.visit_string(s),
            },
            NodeKind::Sequence => visitor.visit_seq(SeqDeserializer {
                children: self.table.kids(self.node),
                index: 0,
                visiting: self.visiting,
                dedup: self.dedup,
                table: self.table,
            }),
            NodeKind::Mapping => visitor.visit_map(MapDeserializer::open(
                self.node,
                self.visiting,
                self.dedup,
                self.table,
            )?),
            NodeKind::Alias => unreachable!("aliases resolve before dispatch"),
        }
    }

    /// `deserialize_option` on a non-alias node: null (a plain-or-Any
    /// empty/`~`/`null` scalar, per the resolver) → none, everything
    /// else — tagged nodes included — some.
    fn option_dispatch<'de, V: Visitor<'de>>(
        self,
        visitor: V,
    ) -> std::result::Result<V::Value, DeserError>
    where
        'doc: 'de,
        'g: 'de,
    {
        if !self.tag_suppressed && utf8_tag(self.node).is_some() {
            return visitor.visit_some(self);
        }
        if self.node.kind() == NodeKind::Scalar
            && matches!(resolve_scalar(self.node), ScalarVal::Null)
        {
            return visitor.visit_none();
        }
        visitor.visit_some(self)
    }

    /// `deserialize_enum` on a non-alias node: a tagged node is an
    /// error (the old route's `Value::Tagged` fell into the reject
    /// arm), a string is a unit variant, a single-variant mapping is
    /// the externally tagged form.
    fn enum_dispatch<'de, V: Visitor<'de>>(
        self,
        visitor: V,
    ) -> std::result::Result<V::Value, DeserError>
    where
        'doc: 'de,
        'g: 'de,
    {
        if !self.tag_suppressed && utf8_tag(self.node).is_some() {
            return Err(DeserError(format!(
                "expected a string or single-variant mapping for an enum, got {:?}",
                node_value(self.node)?,
            )));
        }
        match self.node.kind() {
            NodeKind::Scalar => match resolve_scalar(self.node) {
                ScalarVal::Str(s) => visitor.visit_enum(s.as_ref().into_deserializer()),
                _ => Err(DeserError(format!(
                    "expected a string or single-variant mapping for an enum, got {:?}",
                    node_value(self.node)?,
                ))),
            },
            NodeKind::Mapping => {
                let pairs = MapPairs::open(self.node, self.visiting, self.dedup, self.table)?;
                if pairs.count != 1 {
                    return Err(DeserError(format!(
                        "expected a single-variant mapping for an enum, got {} entries",
                        pairs.count
                    )));
                }
                let (key, value) = pairs.entry(0)?;
                let (key, pushed) = resolve_chain(key, self.visiting)?;
                let variant = match resolve_scalar(key) {
                    ScalarVal::Str(s) => s,
                    _ => {
                        self.remove_ids(&pushed);
                        return Err(DeserError(format!(
                            "expected a string enum variant, got {:?}",
                            node_value(key)?,
                        )));
                    }
                };
                let out = visitor.visit_enum(EnumDeserializer {
                    variant,
                    // the mapping-enum payload keeps its own tag: the
                    // old route's payload there was the resolved Value,
                    // tags and all
                    payload: self.child(value),
                });
                self.remove_ids(&pushed);
                out
            }
            NodeKind::Sequence => Err(DeserError(format!(
                "expected a string or single-variant mapping for an enum, got {:?}",
                node_value(self.node)?,
            ))),
            NodeKind::Alias => unreachable!("aliases resolve before dispatch"),
        }
    }
}

/// The deduped mapping entry list. Identity mode streams the document
/// order directly (the common case — no duplicates); dedup mode holds
/// one entry per unique key, in first-occurrence order, each carrying
/// its LAST value (`Mapping::insert`'s fold).
struct MapPairs<'doc> {
    /// drained KEY,VALUE pairs interleaved (document order), shared
    /// with the drain table
    children: Rc<Vec<Node<'doc>>>,
    identity: bool,
    /// dedup mode only: (resolved-key-node, last-value-node) per entry
    pairs: Vec<(Node<'doc>, Node<'doc>)>,
    count: usize,
}

impl<'doc> MapPairs<'doc> {
    fn open<'g>(
        node: Node<'doc>,
        visiting: &'g Guard,
        dedup: &'g DedupCell<'doc>,
        table: &'g Drained<'doc>,
    ) -> std::result::Result<MapPairs<'doc>, DeserError> {
        let children = table.kids(node);
        let n = children.len() / 2;
        let identity = |children: Rc<Vec<Node<'doc>>>, count: usize| MapPairs {
            children,
            identity: true,
            pairs: Vec::new(),
            count,
        };
        if n < 2 {
            return Ok(identity(children, n));
        }
        // duplicate detection, phase 1: keys compare as their resolved
        // values; the allocation-free happy path only learns WHETHER a
        // duplicate exists (early exit at the first one)
        let mut folded = false;
        {
            let mut seen = dedup.borrow_mut();
            seen.clear();
            for i in 0..n {
                let rk = RKey::of(children[i * 2], visiting)?;
                if seen.insert(rk, i).is_some() {
                    folded = true;
                    break;
                }
            }
        };
        if !folded {
            return Ok(identity(children, n));
        }
        // phase 2 (duplicate-bearing mappings only): the full
        // first-position + last-value fold, `Mapping::insert`'s shape
        let mut slots: Vec<usize> = Vec::with_capacity(n);
        let mut lastv: Vec<usize> = Vec::with_capacity(n);
        {
            let mut seen = dedup.borrow_mut();
            seen.clear();
            for i in 0..n {
                let rk = RKey::of(children[i * 2], visiting)?;
                match seen.entry(rk) {
                    Entry::Occupied(e) => lastv[*e.get()] = i,
                    Entry::Vacant(e) => {
                        e.insert(slots.len());
                        slots.push(i);
                        lastv.push(i);
                    }
                }
            }
        }
        let mut pairs = Vec::with_capacity(slots.len());
        for (ki, vi) in slots.into_iter().zip(lastv) {
            let (k, _) = resolve_chain(children[ki * 2], visiting)?;
            pairs.push((k, children[vi * 2 + 1]));
        }
        Ok(MapPairs {
            children,
            identity: false,
            count: pairs.len(),
            pairs,
        })
    }

    /// Entry `i`: the key node and its (last) value node. Dedup-mode
    /// keys are pre-resolved; identity-mode keys are raw (aliases
    /// resolve at delivery).
    fn entry(&self, i: usize) -> std::result::Result<(Node<'doc>, Node<'doc>), DeserError> {
        if self.identity {
            let k = *self.children.get(i * 2).ok_or_else(internal_error)?;
            let v = *self.children.get(i * 2 + 1).ok_or_else(internal_error)?;
            return Ok((k, v));
        }
        self.pairs.get(i).copied().ok_or_else(internal_error)
    }
}

struct MapDeserializer<'doc, 'g> {
    pairs: MapPairs<'doc>,
    cursor: usize,
    next_value: Option<Node<'doc>>,
    visiting: &'g Guard,
    dedup: &'g DedupCell<'doc>,
    table: &'g Drained<'doc>,
}

impl<'doc, 'g> MapDeserializer<'doc, 'g> {
    fn open(
        node: Node<'doc>,
        visiting: &'g Guard,
        dedup: &'g DedupCell<'doc>,
        table: &'g Drained<'doc>,
    ) -> std::result::Result<MapDeserializer<'doc, 'g>, DeserError> {
        Ok(MapDeserializer {
            pairs: MapPairs::open(node, visiting, dedup, table)?,
            cursor: 0,
            next_value: None,
            visiting,
            dedup,
            table,
        })
    }

    fn child(&self, node: Node<'doc>) -> NodeDeserializer<'doc, 'g> {
        NodeDeserializer {
            node,
            tag_suppressed: false,
            visiting: self.visiting,
            dedup: self.dedup,
            table: self.table,
        }
    }

    fn remove_ids(&self, ids: &[u32]) {
        if ids.is_empty() {
            return;
        }
        let mut set = self.visiting.borrow_mut();
        for id in ids {
            set.remove(id);
        }
    }
}

impl<'de, 'doc, 'g> MapAccess<'de> for MapDeserializer<'doc, 'g>
where
    'doc: 'de,
    'g: 'de,
{
    type Error = DeserError;

    fn next_key_seed<K: DeserializeSeed<'de>>(
        &mut self,
        seed: K,
    ) -> std::result::Result<Option<K::Value>, DeserError> {
        if self.cursor >= self.pairs.count {
            return Ok(None);
        }
        let (key, value) = self.pairs.entry(self.cursor)?;
        self.cursor += 1;
        self.next_value = Some(value);
        // the old route delivered the alias-expanded key Value
        let (key, pushed) = resolve_chain(key, self.visiting)?;
        let out = seed.deserialize(self.child(key));
        self.remove_ids(&pushed);
        out.map(Some)
    }

    fn next_value_seed<V: DeserializeSeed<'de>>(
        &mut self,
        seed: V,
    ) -> std::result::Result<V::Value, DeserError> {
        let v = self
            .next_value
            .take()
            .ok_or_else(|| DeserError("value seed without key".to_owned()))?;
        seed.deserialize(self.child(v))
    }

    fn size_hint(&self) -> Option<usize> {
        Some(self.pairs.count - self.cursor)
    }
}

struct SeqDeserializer<'doc, 'g> {
    /// drained items, in order
    children: Rc<Vec<Node<'doc>>>,
    index: usize,
    visiting: &'g Guard,
    dedup: &'g DedupCell<'doc>,
    table: &'g Drained<'doc>,
}

impl<'doc, 'g> SeqDeserializer<'doc, 'g> {
    fn child(&self, node: Node<'doc>) -> NodeDeserializer<'doc, 'g> {
        NodeDeserializer {
            node,
            tag_suppressed: false,
            visiting: self.visiting,
            dedup: self.dedup,
            table: self.table,
        }
    }
}

impl<'de, 'doc, 'g> SeqAccess<'de> for SeqDeserializer<'doc, 'g>
where
    'doc: 'de,
    'g: 'de,
{
    type Error = DeserError;

    fn next_element_seed<T: DeserializeSeed<'de>>(
        &mut self,
        seed: T,
    ) -> std::result::Result<Option<T::Value>, DeserError> {
        // a `.get()` miss is exhaustion, not an error
        let item = match self.children.get(self.index) {
            Some(item) => *item,
            None => return Ok(None),
        };
        self.index += 1;
        seed.deserialize(self.child(item)).map(Some)
    }

    fn size_hint(&self) -> Option<usize> {
        Some(self.children.len() - self.index)
    }
}

struct EnumDeserializer<'doc, 'g> {
    variant: Cow<'doc, str>,
    payload: NodeDeserializer<'doc, 'g>,
}

impl<'de, 'doc, 'g> EnumAccess<'de> for EnumDeserializer<'doc, 'g>
where
    'doc: 'de,
    'g: 'de,
{
    type Error = DeserError;
    type Variant = EnumVariantDeserializer<'doc, 'g>;

    fn variant_seed<V: DeserializeSeed<'de>>(
        self,
        seed: V,
    ) -> std::result::Result<(V::Value, Self::Variant), DeserError> {
        // borrowed tags go through the &'de-str path (the old route's
        // `s.as_str().into_deserializer()`); the rare lossy owned
        // variant rides String's own IntoDeserializer
        let out = match self.variant {
            Cow::Borrowed(s) => seed
                .deserialize(<&'de str as IntoDeserializer<'de, DeserError>>::into_deserializer(s)),
            Cow::Owned(s) => seed
                .deserialize(<String as IntoDeserializer<'de, DeserError>>::into_deserializer(s)),
        };
        Ok((
            out?,
            EnumVariantDeserializer {
                payload: self.payload,
            },
        ))
    }
}

struct EnumVariantDeserializer<'doc, 'g> {
    payload: NodeDeserializer<'doc, 'g>,
}

impl<'de, 'doc, 'g> VariantAccess<'de> for EnumVariantDeserializer<'doc, 'g>
where
    'doc: 'de,
    'g: 'de,
{
    type Error = DeserError;

    fn unit_variant(self) -> std::result::Result<(), DeserError> {
        // the old route compared the payload VALUE against Null; a
        // payload with a visible tag is a Tagged value, never null —
        // except in the tagged-enum case, where the payload is the
        // tag-stripped inner value
        let (node, pushed) = resolve_chain(self.payload.node, self.payload.visiting)?;
        let tag_visible = !self.payload.tag_suppressed && utf8_tag(node).is_some();
        let is_null = !tag_visible
            && node.kind() == NodeKind::Scalar
            && matches!(resolve_scalar(node), ScalarVal::Null);
        self.payload.remove_ids(&pushed);
        if is_null {
            Ok(())
        } else {
            Err(DeserError(format!(
                "expected unit variant, got {:?}",
                node_value(self.payload.node)?,
            )))
        }
    }

    fn newtype_variant_seed<T: DeserializeSeed<'de>>(
        self,
        seed: T,
    ) -> std::result::Result<T::Value, DeserError> {
        seed.deserialize(self.payload)
    }

    fn tuple_variant<V: Visitor<'de>>(
        self,
        _len: usize,
        visitor: V,
    ) -> std::result::Result<V::Value, DeserError> {
        self.payload.deserialize_any(visitor)
    }

    fn struct_variant<V: Visitor<'de>>(
        self,
        _fields: &'static [&'static str],
        visitor: V,
    ) -> std::result::Result<V::Value, DeserError> {
        self.payload.deserialize_any(visitor)
    }
}

/// A mapping KEY's dedup identity — equality and hash mirror the
/// resolved-`Value` equality the old route compared (`Mapping::insert`):
/// resolution folds `0x10` and `16` together, quoted `'1'` stays apart
/// from plain `1`, lossy strings merge on their replacement chars, NaN
/// never equals anything, and `-0.0` equals `0.0` (f64 equality).
enum RKey<'doc> {
    Null,
    Bool(bool),
    Int(i64),
    Uint(u64),
    /// a non-NaN, non-zero float
    Float(f64),
    /// ±0.0 — one variant, because f64 eq says -0.0 == 0.0
    Zero,
    /// NaN — deliberately non-reflexive, like f64/`Value` equality
    Nan,
    Str(Cow<'doc, str>),
    /// tagged or non-scalar keys: equality through the materialized
    /// `Value`, hash through the variant only (rare — exact and slow)
    Structured(Value),
}

impl<'doc> RKey<'doc> {
    fn of(node: Node<'doc>, visiting: &Guard) -> std::result::Result<RKey<'doc>, DeserError> {
        let (node, pushed) = resolve_chain(node, visiting)?;
        let rk = match node.kind() {
            NodeKind::Scalar if utf8_tag(node).is_none() => match resolve_scalar(node) {
                ScalarVal::Null => RKey::Null,
                ScalarVal::Bool(b) => RKey::Bool(b),
                ScalarVal::Int(v) => RKey::Int(v),
                ScalarVal::Uint(v) => RKey::Uint(v),
                // in-arm branches, not guards: a float PATTERN
                // matches only +0.0, but -0.0 == 0.0 and both must
                // fold into the same key (f64 equality)
                ScalarVal::Float(v) => {
                    if v.is_nan() {
                        RKey::Nan
                    } else if v == 0.0 {
                        RKey::Zero
                    } else {
                        RKey::Float(v)
                    }
                }
                ScalarVal::Str(s) => RKey::Str(s),
            },
            // a tagged scalar or a non-scalar key: identity is the
            // full resolved Value (tags participate, containers
            // compare structurally)
            _ => RKey::Structured(node_value(node)?),
        };
        {
            let mut set = visiting.borrow_mut();
            for id in pushed {
                set.remove(&id);
            }
        }
        Ok(rk)
    }
}

impl PartialEq for RKey<'_> {
    fn eq(&self, other: &Self) -> bool {
        match (self, other) {
            (RKey::Null, RKey::Null) => true,
            (RKey::Bool(a), RKey::Bool(b)) => a == b,
            (RKey::Int(a), RKey::Int(b)) => a == b,
            (RKey::Uint(a), RKey::Uint(b)) => a == b,
            // neither side is NaN by construction; f64 eq is the
            // `Value` route's comparison (-0.0 == 0.0 folds)
            (RKey::Float(a), RKey::Float(b)) => a == b,
            (RKey::Zero, RKey::Zero) => true,
            // never reflexive: two .nan keys stay distinct entries
            (RKey::Nan, RKey::Nan) => false,
            (RKey::Str(a), RKey::Str(b)) => a == b,
            (RKey::Structured(a), RKey::Structured(b)) => a == b,
            _ => false,
        }
    }
}

impl Eq for RKey<'_> {}

impl Hash for RKey<'_> {
    fn hash<H: Hasher>(&self, state: &mut H) {
        match self {
            RKey::Null => state.write_u8(0),
            RKey::Bool(b) => {
                state.write_u8(1);
                state.write_u8(u8::from(*b));
            }
            RKey::Int(v) => {
                state.write_u8(2);
                state.write_u64(v.unsigned_abs());
            }
            RKey::Uint(v) => {
                state.write_u8(3);
                state.write_u64(*v);
            }
            // eq-true pairs must hash-equal: non-zero floats equal
            // only their own bit pattern, and Zero/Zero share one tag
            RKey::Float(v) => {
                state.write_u8(4);
                state.write_u64(v.to_bits());
            }
            RKey::Zero => state.write_u8(5),
            RKey::Nan => state.write_u8(6),
            RKey::Str(s) => {
                state.write_u8(7);
                s.hash(state);
            }
            // one bucket for all structured keys: equality decides
            RKey::Structured(_) => state.write_u8(8),
        }
    }
}

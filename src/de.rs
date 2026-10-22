//! The serde deserialization face: `from_str` / `from_slice` /
//! `from_value` — a `Deserializer` over the materialized [`Value`]
//! tree (serde_yaml's architecture; a zero-copy event-stream
//! deserializer is the measured follow-up per the perf ledger
//! discipline).

use crate::value::{Mapping, Number, Tagged, Value};
use crate::{Document, Error};
use serde::de::{
    self, DeserializeOwned, DeserializeSeed, EnumAccess, IntoDeserializer, MapAccess, SeqAccess,
    VariantAccess, Visitor,
};
use serde::Deserializer as _;

#[derive(Debug)]
pub struct DeserError(String);

impl std::fmt::Display for DeserError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for DeserError {}

impl de::Error for DeserError {
    fn custom<T: std::fmt::Display>(msg: T) -> Self {
        DeserError(msg.to_string())
    }
}

impl From<Error> for DeserError {
    fn from(e: Error) -> Self {
        DeserError(e.to_string())
    }
}

/// Deserialize `T` from a YAML string (serde_yaml parity: exactly one
/// document; a multi-document stream is an error).
pub fn from_str<T: DeserializeOwned>(s: &str) -> std::result::Result<T, DeserError> {
    from_slice(s.as_bytes())
}

/// Byte form of [`from_str`].
pub fn from_slice<T: DeserializeOwned>(s: &[u8]) -> std::result::Result<T, DeserError> {
    let doc = Document::parse(s)?;
    if doc.document_count() != 1 {
        return Err(DeserError(format!(
            "deserializing from YAML containing more than one document is not supported: {} documents",
            doc.document_count()
        )));
    }
    let value = Value::from_document(&doc)?;
    from_value(&value)
}

/// Deserialize `T` from an owned [`Value`].
pub fn from_value<T: DeserializeOwned>(value: &Value) -> std::result::Result<T, DeserError> {
    T::deserialize(ValueDeserializer(value))
}

struct ValueDeserializer<'a>(&'a Value);

impl<'de, 'a: 'de> de::Deserializer<'de> for ValueDeserializer<'a> {
    type Error = DeserError;

    fn deserialize_any<V: Visitor<'de>>(
        self,
        visitor: V,
    ) -> std::result::Result<V::Value, DeserError> {
        match self.0 {
            Value::Null => visitor.visit_unit(),
            Value::Bool(b) => visitor.visit_bool(*b),
            Value::Number(Number::I64(v)) => visitor.visit_i64(*v),
            Value::Number(Number::U64(v)) => visitor.visit_u64(*v),
            Value::Number(Number::F64(v)) => visitor.visit_f64(*v),
            Value::String(s) => visitor.visit_borrowed_str(s), // 'de = 'a; Value outlives
            Value::Sequence(items) => visitor.visit_seq(SeqDeserializer { iter: items.iter() }),
            Value::Mapping(map) => visitor.visit_map(MapDeserializer {
                iter: map.0.iter(),
                next_value: None,
            }),
            Value::Tagged(t) => {
                // serde_yaml's rule: a tagged node visits as an ENUM
                // whose variant is the tag — Value's visitor rebuilds
                // Value::Tagged; typed non-enum visitors reject it,
                // the serde_yaml contract.
                visitor.visit_enum(EnumDeserializer {
                    variant: &t.tag,
                    payload: &t.value,
                })
            }
        }
    }

    fn deserialize_option<V: Visitor<'de>>(
        self,
        visitor: V,
    ) -> std::result::Result<V::Value, DeserError> {
        match self.0 {
            Value::Null => visitor.visit_none(),
            _ => visitor.visit_some(self),
        }
    }

    fn deserialize_enum<V: Visitor<'de>>(
        self,
        _name: &str,
        _variants: &[&str],
        visitor: V,
    ) -> std::result::Result<V::Value, DeserError> {
        match self.0 {
            Value::String(s) => visitor.visit_enum(s.as_str().into_deserializer()),
            Value::Mapping(map) => {
                // { variant: payload } — the externally tagged form
                if map.0.len() != 1 {
                    return Err(DeserError(format!(
                        "expected a single-variant mapping for an enum, got {} entries",
                        map.0.len()
                    )));
                }
                let (k, v) = &map.0[0];
                let variant = match k {
                    Value::String(s) => s.as_str(),
                    other => {
                        return Err(DeserError(format!(
                            "expected a string enum variant, got {:?}",
                            other
                        )))
                    }
                };
                visitor.visit_enum(EnumDeserializer {
                    variant,
                    payload: v,
                })
            }
            v => Err(DeserError(format!(
                "expected a string or single-variant mapping for an enum, got {:?}",
                v
            ))),
        }
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

struct SeqDeserializer<'a> {
    iter: std::slice::Iter<'a, Value>,
}

impl<'de, 'a: 'de> SeqAccess<'de> for SeqDeserializer<'a> {
    type Error = DeserError;
    fn next_element_seed<T: DeserializeSeed<'de>>(
        &mut self,
        seed: T,
    ) -> std::result::Result<Option<T::Value>, DeserError> {
        match self.iter.next() {
            Some(v) => seed.deserialize(ValueDeserializer(v)).map(Some),
            None => Ok(None),
        }
    }
    fn size_hint(&self) -> Option<usize> {
        Some(self.iter.len())
    }
}

struct MapDeserializer<'a> {
    iter: std::slice::Iter<'a, (Value, Value)>,
    next_value: Option<&'a Value>,
}

impl<'de, 'a: 'de> MapAccess<'de> for MapDeserializer<'a> {
    type Error = DeserError;
    fn next_key_seed<K: DeserializeSeed<'de>>(
        &mut self,
        seed: K,
    ) -> std::result::Result<Option<K::Value>, DeserError> {
        match self.iter.next() {
            Some((k, v)) => {
                self.next_value = Some(v);
                seed.deserialize(ValueDeserializer(k)).map(Some)
            }
            None => Ok(None),
        }
    }
    fn next_value_seed<V: DeserializeSeed<'de>>(
        &mut self,
        seed: V,
    ) -> std::result::Result<V::Value, DeserError> {
        let v = self
            .next_value
            .take()
            .ok_or_else(|| DeserError("value seed without key".to_owned()))?;
        seed.deserialize(ValueDeserializer(v))
    }
    fn size_hint(&self) -> Option<usize> {
        Some(self.iter.len())
    }
}

struct EnumDeserializer<'a> {
    variant: &'a str,
    payload: &'a Value,
}

impl<'de, 'a: 'de> EnumAccess<'de> for EnumDeserializer<'a> {
    type Error = DeserError;
    type Variant = EnumVariantDeserializer<'a>;
    fn variant_seed<V: DeserializeSeed<'de>>(
        self,
        seed: V,
    ) -> std::result::Result<(V::Value, Self::Variant), DeserError> {
        let variant_de =
            <&'a str as IntoDeserializer<'a, DeserError>>::into_deserializer(self.variant);
        let variant = seed.deserialize(variant_de)?;
        Ok((
            variant,
            EnumVariantDeserializer {
                payload: self.payload,
            },
        ))
    }
}

struct EnumVariantDeserializer<'a> {
    payload: &'a Value,
}

impl<'de, 'a: 'de> VariantAccess<'de> for EnumVariantDeserializer<'a> {
    type Error = DeserError;
    fn unit_variant(self) -> std::result::Result<(), DeserError> {
        match self.payload {
            Value::Null => Ok(()),
            v => Err(DeserError(format!("expected unit variant, got {:?}", v))),
        }
    }
    fn newtype_variant_seed<T: DeserializeSeed<'de>>(
        self,
        seed: T,
    ) -> std::result::Result<T::Value, DeserError> {
        seed.deserialize(ValueDeserializer(self.payload))
    }
    fn tuple_variant<V: Visitor<'de>>(
        self,
        len: usize,
        visitor: V,
    ) -> std::result::Result<V::Value, DeserError> {
        let _ = len;
        ValueDeserializer(self.payload).deserialize_any(visitor)
    }
    fn struct_variant<V: Visitor<'de>>(
        self,
        _fields: &'static [&'static str],
        visitor: V,
    ) -> std::result::Result<V::Value, DeserError> {
        ValueDeserializer(self.payload).deserialize_any(visitor)
    }
}

// Value itself is Deserialize (round-trip through from_value)
impl<'de> serde::Deserialize<'de> for Value {
    fn deserialize<D: serde::Deserializer<'de>>(
        deserializer: D,
    ) -> std::result::Result<Self, D::Error> {
        struct ValueVisitor;
        impl<'de> Visitor<'de> for ValueVisitor {
            type Value = Value;
            fn expecting(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                f.write_str("any YAML value")
            }
            fn visit_unit<E: de::Error>(self) -> std::result::Result<Value, E> {
                Ok(Value::Null)
            }
            fn visit_bool<E: de::Error>(self, v: bool) -> std::result::Result<Value, E> {
                Ok(Value::Bool(v))
            }
            fn visit_i64<E: de::Error>(self, v: i64) -> std::result::Result<Value, E> {
                Ok(Value::Number(Number::I64(v)))
            }
            fn visit_u64<E: de::Error>(self, v: u64) -> std::result::Result<Value, E> {
                Ok(Value::Number(Number::U64(v)))
            }
            fn visit_f64<E: de::Error>(self, v: f64) -> std::result::Result<Value, E> {
                Ok(Value::Number(Number::F64(v)))
            }
            fn visit_str<E: de::Error>(self, v: &str) -> std::result::Result<Value, E> {
                Ok(Value::String(v.to_owned()))
            }
            fn visit_string<E: de::Error>(self, v: String) -> std::result::Result<Value, E> {
                Ok(Value::String(v))
            }
            fn visit_none<E: de::Error>(self) -> std::result::Result<Value, E> {
                Ok(Value::Null)
            }
            fn visit_some<D: serde::Deserializer<'de>>(
                self,
                d: D,
            ) -> std::result::Result<Value, D::Error> {
                serde::Deserialize::deserialize(d)
            }
            fn visit_seq<A: SeqAccess<'de>>(
                self,
                mut seq: A,
            ) -> std::result::Result<Value, A::Error> {
                let mut items = Vec::new();
                while let Some(v) = seq.next_element()? {
                    items.push(v);
                }
                Ok(Value::Sequence(items))
            }
            fn visit_map<A: MapAccess<'de>>(
                self,
                mut map: A,
            ) -> std::result::Result<Value, A::Error> {
                let mut mapping = Mapping::new();
                while let Some((k, v)) = map.next_entry()? {
                    mapping.insert(k, v);
                }
                Ok(Value::Mapping(mapping))
            }
            fn visit_enum<A: EnumAccess<'de>>(
                self,
                data: A,
            ) -> std::result::Result<Value, A::Error> {
                let (tag, variant): (String, _) = data.variant()?;
                let value: Value = variant.newtype_variant()?;
                Ok(Value::Tagged(Box::new(Tagged { tag, value })))
            }
        }
        deserializer.deserialize_any(ValueVisitor)
    }
}

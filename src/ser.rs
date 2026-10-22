//! `Serialize for Value` plus the `Serialize → Value` serializer
//! (`to_value`), so any `Serialize` type can reach [`crate::to_string`]
//! without an intermediate JSON step.

use crate::value::{Mapping, Number, Value};
use serde::ser::{self, SerializeMap};
use serde::{Serialize, Serializer};

impl Serialize for Value {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        match self {
            Value::Null => serializer.serialize_unit(),
            Value::Bool(b) => serializer.serialize_bool(*b),
            Value::Number(Number::I64(v)) => serializer.serialize_i64(*v),
            Value::Number(Number::U64(v)) => serializer.serialize_u64(*v),
            Value::Number(Number::F64(v)) => serializer.serialize_f64(*v),
            Value::String(s) => serializer.serialize_str(s),
            Value::Sequence(items) => items.serialize(serializer),
            Value::Mapping(map) => {
                let mut ser_map = serializer.serialize_map(Some(map.0.len()))?;
                for (k, v) in &map.0 {
                    ser_map.serialize_entry(k, v)?;
                }
                ser_map.end()
            }
            Value::Tagged(t) => {
                // serde_yaml's convention: a tagged value serializes as
                // a one-entry mapping { tag => value }
                let mut ser_map = serializer.serialize_map(Some(1))?;
                ser_map.serialize_entry(&t.tag[..], &t.value)?;
                ser_map.end()
            }
        }
    }
}

/// Convert any `Serialize` into a [`Value`].
pub fn to_value<T: Serialize + ?Sized>(value: &T) -> Result<Value, ToValueError> {
    value.serialize(ValueSerializer)
}

#[derive(Debug)]
pub struct ToValueError(String);

impl std::fmt::Display for ToValueError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for ToValueError {}

impl serde::ser::Error for ToValueError {
    fn custom<T: std::fmt::Display>(msg: T) -> Self {
        ToValueError(msg.to_string())
    }
}

struct ValueSerializer;

impl Serializer for ValueSerializer {
    type Ok = Value;
    type Error = ToValueError;
    type SerializeSeq = SeqSer;
    type SerializeTuple = SeqSer;
    type SerializeTupleStruct = SeqSer;
    type SerializeTupleVariant = VariantSeqSer;
    type SerializeMap = MapSer;
    type SerializeStruct = MapSer;
    type SerializeStructVariant = VariantMapSer;

    fn serialize_bool(self, v: bool) -> Result<Value, Self::Error> {
        Ok(Value::Bool(v))
    }
    fn serialize_i8(self, v: i8) -> Result<Value, Self::Error> {
        self.serialize_i64(v as i64)
    }
    fn serialize_i16(self, v: i16) -> Result<Value, Self::Error> {
        self.serialize_i64(v as i64)
    }
    fn serialize_i32(self, v: i32) -> Result<Value, Self::Error> {
        self.serialize_i64(v as i64)
    }
    fn serialize_i64(self, v: i64) -> Result<Value, Self::Error> {
        Ok(Value::Number(Number::I64(v)))
    }
    fn serialize_u8(self, v: u8) -> Result<Value, Self::Error> {
        self.serialize_u64(v as u64)
    }
    fn serialize_u16(self, v: u16) -> Result<Value, Self::Error> {
        self.serialize_u64(v as u64)
    }
    fn serialize_u32(self, v: u32) -> Result<Value, Self::Error> {
        self.serialize_u64(v as u64)
    }
    fn serialize_u64(self, v: u64) -> Result<Value, Self::Error> {
        Ok(Value::Number(Number::U64(v)))
    }
    fn serialize_f32(self, v: f32) -> Result<Value, Self::Error> {
        self.serialize_f64(v as f64)
    }
    fn serialize_f64(self, v: f64) -> Result<Value, Self::Error> {
        Ok(Value::Number(Number::F64(v)))
    }
    fn serialize_char(self, v: char) -> Result<Value, Self::Error> {
        Ok(Value::String(v.to_string()))
    }
    fn serialize_str(self, v: &str) -> Result<Value, Self::Error> {
        Ok(Value::String(v.to_owned()))
    }
    fn serialize_bytes(self, v: &[u8]) -> Result<Value, Self::Error> {
        // serde_yaml has no byte type; strings are the honest carrier
        Ok(Value::String(String::from_utf8_lossy(v).into_owned()))
    }
    fn serialize_none(self) -> Result<Value, Self::Error> {
        Ok(Value::Null)
    }
    fn serialize_some<T: Serialize + ?Sized>(self, value: &T) -> Result<Value, Self::Error> {
        value.serialize(self)
    }
    fn serialize_unit(self) -> Result<Value, Self::Error> {
        Ok(Value::Null)
    }
    fn serialize_unit_struct(self, _name: &'static str) -> Result<Value, Self::Error> {
        Ok(Value::Null)
    }
    fn serialize_unit_variant(
        self,
        _name: &'static str,
        _index: u32,
        variant: &'static str,
    ) -> Result<Value, Self::Error> {
        Ok(Value::String(variant.to_owned()))
    }
    fn serialize_newtype_struct<T: Serialize + ?Sized>(
        self,
        _name: &'static str,
        value: &T,
    ) -> Result<Value, Self::Error> {
        value.serialize(self)
    }
    fn serialize_newtype_variant<T: Serialize + ?Sized>(
        self,
        _name: &'static str,
        _index: u32,
        variant: &'static str,
        value: &T,
    ) -> Result<Value, Self::Error> {
        let mut map = Mapping::new();
        map.insert(
            Value::String(variant.to_owned()),
            value.serialize(ValueSerializer)?,
        );
        Ok(Value::Mapping(map))
    }
    fn serialize_seq(self, len: Option<usize>) -> Result<Self::SerializeSeq, Self::Error> {
        Ok(SeqSer {
            items: Vec::with_capacity(len.unwrap_or(0)),
        })
    }
    fn serialize_tuple(self, len: usize) -> Result<Self::SerializeTuple, Self::Error> {
        self.serialize_seq(Some(len))
    }
    fn serialize_tuple_struct(
        self,
        _name: &'static str,
        len: usize,
    ) -> Result<Self::SerializeTupleStruct, Self::Error> {
        self.serialize_seq(Some(len))
    }
    fn serialize_tuple_variant(
        self,
        _name: &'static str,
        _index: u32,
        _variant: &'static str,
        len: usize,
    ) -> Result<Self::SerializeTupleVariant, Self::Error> {
        Ok(VariantSeqSer {
            variant: _variant.to_owned(),
            items: Vec::with_capacity(len),
        })
    }
    fn serialize_map(self, _len: Option<usize>) -> Result<Self::SerializeMap, Self::Error> {
        Ok(MapSer {
            mapping: Mapping::new(),
            next_key: None,
        })
    }
    fn serialize_struct(
        self,
        _name: &'static str,
        len: usize,
    ) -> Result<Self::SerializeStruct, Self::Error> {
        let _ = len;
        Ok(MapSer {
            mapping: Mapping::new(),
            next_key: None,
        })
    }
    fn serialize_struct_variant(
        self,
        _name: &'static str,
        _index: u32,
        _variant: &'static str,
        len: usize,
    ) -> Result<Self::SerializeStructVariant, Self::Error> {
        let _ = len;
        Ok(VariantMapSer {
            variant: _variant.to_owned(),
            inner: MapSer {
                mapping: Mapping::new(),
                next_key: None,
            },
        })
    }
}

struct SeqSer {
    items: Vec<Value>,
}

impl ser::SerializeSeq for SeqSer {
    type Ok = Value;
    type Error = ToValueError;
    fn serialize_element<T: Serialize + ?Sized>(&mut self, value: &T) -> Result<(), Self::Error> {
        self.items.push(value.serialize(ValueSerializer)?);
        Ok(())
    }
    fn end(self) -> Result<Value, Self::Error> {
        Ok(Value::Sequence(self.items))
    }
}

impl ser::SerializeTuple for SeqSer {
    type Ok = Value;
    type Error = ToValueError;
    fn serialize_element<T: Serialize + ?Sized>(&mut self, value: &T) -> Result<(), Self::Error> {
        ser::SerializeSeq::serialize_element(self, value)
    }
    fn end(self) -> Result<Value, Self::Error> {
        ser::SerializeSeq::end(self)
    }
}

impl ser::SerializeTupleStruct for SeqSer {
    type Ok = Value;
    type Error = ToValueError;
    fn serialize_field<T: Serialize + ?Sized>(&mut self, value: &T) -> Result<(), Self::Error> {
        ser::SerializeSeq::serialize_element(self, value)
    }
    fn end(self) -> Result<Value, Self::Error> {
        ser::SerializeSeq::end(self)
    }
}

struct VariantSeqSer {
    variant: String,
    items: Vec<Value>,
}

impl ser::SerializeTupleVariant for VariantSeqSer {
    type Ok = Value;
    type Error = ToValueError;
    fn serialize_field<T: Serialize + ?Sized>(&mut self, value: &T) -> Result<(), Self::Error> {
        self.items.push(value.serialize(ValueSerializer)?);
        Ok(())
    }
    fn end(self) -> Result<Value, Self::Error> {
        let mut map = Mapping::new();
        map.insert(Value::String(self.variant), Value::Sequence(self.items));
        Ok(Value::Mapping(map))
    }
}

struct MapSer {
    mapping: Mapping,
    next_key: Option<Value>,
}

impl ser::SerializeMap for MapSer {
    type Ok = Value;
    type Error = ToValueError;
    fn serialize_key<T: Serialize + ?Sized>(&mut self, key: &T) -> Result<(), Self::Error> {
        self.next_key = Some(key.serialize(ValueSerializer)?);
        Ok(())
    }
    fn serialize_value<T: Serialize + ?Sized>(&mut self, value: &T) -> Result<(), Self::Error> {
        let key = self
            .next_key
            .take()
            .ok_or_else(|| ToValueError("serialize_value before serialize_key".to_owned()))?;
        self.mapping.insert(key, value.serialize(ValueSerializer)?);
        Ok(())
    }
    fn end(self) -> Result<Value, Self::Error> {
        Ok(Value::Mapping(self.mapping))
    }
}

impl ser::SerializeStruct for MapSer {
    type Ok = Value;
    type Error = ToValueError;
    fn serialize_field<T: Serialize + ?Sized>(
        &mut self,
        key: &'static str,
        value: &T,
    ) -> Result<(), Self::Error> {
        ser::SerializeMap::serialize_key(self, key)?;
        ser::SerializeMap::serialize_value(self, value)
    }
    fn end(self) -> Result<Value, Self::Error> {
        ser::SerializeMap::end(self)
    }
}

struct VariantMapSer {
    variant: String,
    inner: MapSer,
}

impl ser::SerializeStructVariant for VariantMapSer {
    type Ok = Value;
    type Error = ToValueError;
    fn serialize_field<T: Serialize + ?Sized>(
        &mut self,
        key: &'static str,
        value: &T,
    ) -> Result<(), Self::Error> {
        ser::SerializeStruct::serialize_field(&mut self.inner, key, value)
    }
    fn end(self) -> Result<Value, Self::Error> {
        let mut map = Mapping::new();
        map.insert(
            Value::String(self.variant),
            ser::SerializeStruct::end(self.inner)?,
        );
        Ok(Value::Mapping(map))
    }
}

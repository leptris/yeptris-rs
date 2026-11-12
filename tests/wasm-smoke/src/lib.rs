use wasm_bindgen::prelude::*;

#[wasm_bindgen]
pub fn smoke() -> Result<String, JsValue> {
    for _ in 0..128 {
        let input = format!(
            "name: yeptris\nfloat: 1.25\nitems: [true, null, 42]\nlarge: {}\n",
            "a".repeat(16384)
        );
        let value: yeptris::Value =
            yeptris::from_str(&input).map_err(|e| JsValue::from_str(&e.to_string()))?;
        let output = yeptris::to_string(&value).map_err(|e| JsValue::from_str(&e.to_string()))?;
        let reparsed: yeptris::Value =
            yeptris::from_str(&output).map_err(|e| JsValue::from_str(&e.to_string()))?;
        if value != reparsed {
            return Err(JsValue::from_str("parse/emit value mismatch"));
        }
        if yeptris::from_str::<yeptris::Value>("bad: [").is_ok() {
            return Err(JsValue::from_str("invalid YAML accepted"));
        }
    }
    Ok("ok".to_owned())
}

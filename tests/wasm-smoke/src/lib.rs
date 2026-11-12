use wasm_bindgen::prelude::*;

#[wasm_bindgen]
pub fn smoke() -> Result<String, JsValue> {
    for iteration in 0..128 {
        let input = format!(
            "name: yeptris\nfloat: 1.25\nitems: [true, null, 42]\nlarge: {}\n",
            "a".repeat(if iteration == 0 {
                2 * 1024 * 1024
            } else {
                16384
            })
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
    let error = yeptris::Document::parse(b"bad: [")
        .err()
        .ok_or_else(|| JsValue::from_str("invalid DOM accepted"))?;
    let captured = error.to_string();
    let valid = yeptris::Document::parse(b"valid: true\n")
        .map_err(|e| JsValue::from_str(&e.to_string()))?;
    let _ = valid
        .serialize_string()
        .map_err(|e| JsValue::from_str(&e.to_string()))?;
    if captured.is_empty() || error.to_string() != captured {
        return Err(JsValue::from_str("error snapshot changed"));
    }
    Ok("ok".to_owned())
}

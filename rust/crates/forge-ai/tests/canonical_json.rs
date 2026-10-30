//! The journal and hashes need a fixed key order. `serde_json` keeps `Map` sorted only
//! without its `preserve_order` feature, and features are shared across the workspace:
//! this test fails if any crate turns it on.

#[test]
fn json_maps_stay_sorted() {
    let v: serde_json::Value = serde_json::from_str(r#"{"b": 1, "a": 2, "c": {"z": 0, "y": 1}}"#).unwrap();
    assert_eq!(serde_json::to_string(&v).unwrap(), r#"{"a":2,"b":1,"c":{"y":1,"z":0}}"#);
}

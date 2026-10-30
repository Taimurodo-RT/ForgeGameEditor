//! `schema/v1/` is generated from the Rust types and must match them.
//! After changing a type run `FORGE_AI_BLESS=1 cargo test -p forge-ai --test schema` and commit the files.

use std::fs;
use std::path::Path;

#[test]
fn schema_files_are_current() {
    let dir = Path::new(env!("CARGO_MANIFEST_DIR")).join("schema/v1");
    let bless = std::env::var_os("FORGE_AI_BLESS").is_some();
    let generated = forge_ai::data::json_schemas();
    if bless {
        fs::create_dir_all(&dir).unwrap();
        for e in fs::read_dir(&dir).unwrap() {
            fs::remove_file(e.unwrap().path()).unwrap();
        }
    }
    let mut stale = Vec::new();
    for (name, text) in &generated {
        let path = dir.join(name);
        if bless {
            fs::write(&path, text).unwrap();
        } else if fs::read_to_string(&path).map(|t| t.replace("\r\n", "\n")).ok().as_deref() != Some(text) {
            stale.push(name.clone());
        }
    }
    let mut on_disk: Vec<String> =
        fs::read_dir(&dir).unwrap().map(|e| e.unwrap().file_name().into_string().unwrap()).collect();
    on_disk.sort();
    let mut expected: Vec<String> = generated.iter().map(|(n, _)| n.clone()).collect();
    expected.sort();
    assert_eq!(on_disk, expected, "extra or missing files in schema/v1");
    assert!(stale.is_empty(), "schema/v1 is out of date: {stale:?}; run with FORGE_AI_BLESS=1");
}

#[test]
fn schemas_are_draft_2020_12_and_closed() {
    for (name, text) in forge_ai::data::json_schemas() {
        let v: serde_json::Value = serde_json::from_str(&text).unwrap();
        assert_eq!(v["$schema"], "https://json-schema.org/draft/2020-12/schema", "{name}");
        if name != "scenario.json" {
            assert_eq!(v["additionalProperties"], false, "{name}: unknown fields must be errors");
        }
    }
}

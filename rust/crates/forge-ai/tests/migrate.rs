//! Trial migration of the project format 0 → 1 (stage 0 of docs/ai/spec.md): it passes
//! and leaves the source files byte for byte as they were.

use std::collections::BTreeMap;
use std::fs;
use std::path::{Path, PathBuf};

use forge_ai::migrate::{MigrateError, MigrationReport, migrate_to};
use forge_ai::validate::validate_project;

const VALID: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/data/valid");

/// Relative path → bytes of every file under `dir`.
fn snapshot(dir: &Path) -> BTreeMap<PathBuf, Vec<u8>> {
    let mut out = BTreeMap::new();
    let mut todo = vec![dir.to_owned()];
    while let Some(d) = todo.pop() {
        for e in fs::read_dir(&d).unwrap() {
            let p = e.unwrap().path();
            if p.is_dir() {
                todo.push(p);
            } else {
                out.insert(p.strip_prefix(dir).unwrap().to_owned(), fs::read(&p).unwrap());
            }
        }
    }
    out
}

/// The valid project written in format 0: no `format: 1`, an engine bindings file.
fn format_0(root: &Path) -> PathBuf {
    let src = root.join("format0");
    let _ = fs::remove_dir_all(root);
    fs::create_dir_all(&src).unwrap();
    for (rel, bytes) in snapshot(Path::new(VALID)) {
        let to = src.join(&rel);
        fs::create_dir_all(to.parent().unwrap()).unwrap();
        fs::write(to, bytes).unwrap();
    }
    let project = src.join("project.json");
    let mut v: serde_json::Value = serde_json::from_slice(&fs::read(&project).unwrap()).unwrap();
    v["format"] = 0.into();
    fs::write(&project, serde_json::to_string_pretty(&v).unwrap()).unwrap();
    fs::create_dir_all(src.join("engine")).unwrap();
    fs::write(src.join("engine/godot.bindings.json"), r#"{"steps": {"goto_slot": "res://ai/goto.gd"}}"#).unwrap();
    src
}

#[test]
fn format_0_migrates_and_source_stays() {
    let root = Path::new(env!("CARGO_TARGET_TMPDIR")).join("migrate_ok");
    let src = format_0(&root);
    assert!(validate_project(&src).has_errors(), "format 0 must not open without a migration");
    let before = snapshot(&src);

    let dst = root.join("migrated");
    let report = migrate_to(&src, &dst).unwrap();
    assert_eq!(report, MigrationReport { from: 0, to: 1, removed: vec!["engine/godot.bindings.json".to_owned()] });
    assert_eq!(snapshot(&src), before, "the source must stay untouched");
    assert!(!dst.join("engine").exists());
    assert_eq!(validate_project(&dst).lines(), Vec::<String>::new());

    // project.json is rewritten canonically: sorted keys, 2 spaces, LF, final newline.
    let text = fs::read_to_string(dst.join("project.json")).unwrap();
    assert!(text.ends_with("}\n") && !text.contains('\r'));
    assert!(text.starts_with("{\n  \"buckets\""));
}

#[test]
fn migration_refuses_to_overwrite() {
    let root = Path::new(env!("CARGO_TARGET_TMPDIR")).join("migrate_exists");
    let src = format_0(&root);
    let dst = root.join("taken");
    fs::create_dir_all(&dst).unwrap();
    assert!(matches!(migrate_to(&src, &dst), Err(MigrateError::DestinationExists(_))));
    assert_eq!(fs::read_dir(&dst).unwrap().count(), 0);
}

#[test]
fn current_format_is_copied_as_is() {
    let dst = Path::new(env!("CARGO_TARGET_TMPDIR")).join("migrate_current");
    let _ = fs::remove_dir_all(&dst);
    let report = migrate_to(Path::new(VALID), &dst).unwrap();
    assert_eq!((report.from, report.to), (1, 1));
    assert_eq!(snapshot(&dst), snapshot(Path::new(VALID)));
}

//! Project format migrations (docs/ai/data-model.md, «Версии, миграции и ошибки»).
//!
//! A migration never writes over the source: [`migrate_to`] copies the project into a
//! new folder and migrates the copy. The editor keeps the source as `.backup/<date>/`
//! and swaps the folders; that part belongs to the «Жители» tab.

use std::fmt;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};

/// Current project format.
pub const FORMAT: i64 = 1;

/// The engine bindings file of format 0 (28.10.2026), removed by 0 → 1.
const GODOT_BINDINGS: &str = "engine/godot.bindings.json";

#[derive(Debug)]
pub enum MigrateError {
    Io(PathBuf, io::Error),
    /// `project.json` is missing, is not JSON or has no integer `format`.
    BadProject,
    /// The project is newer than this build.
    TooNew(i64),
    /// The destination already exists; nothing was written.
    DestinationExists(PathBuf),
}

impl fmt::Display for MigrateError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            MigrateError::Io(p, e) => write!(f, "{}: {e}", p.display()),
            MigrateError::BadProject => f.write_str("project.json is missing or has no integer `format`"),
            MigrateError::TooNew(v) => write!(f, "project format {v} is newer than {FORMAT}"),
            MigrateError::DestinationExists(p) => write!(f, "{} already exists", p.display()),
        }
    }
}

impl std::error::Error for MigrateError {}

/// What a migration did.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct MigrationReport {
    pub from: i64,
    pub to: i64,
    /// Files left out of the migrated copy, relative, `/` separators.
    pub removed: Vec<String>,
}

fn io_err(p: &Path) -> impl FnOnce(io::Error) -> MigrateError + '_ {
    move |e| MigrateError::Io(p.to_owned(), e)
}

fn copy_tree(src: &Path, dst: &Path) -> Result<(), MigrateError> {
    fs::create_dir_all(dst).map_err(io_err(dst))?;
    let mut entries: Vec<_> = fs::read_dir(src).map_err(io_err(src))?.filter_map(Result::ok).collect();
    entries.sort_by_key(|e| e.file_name());
    for e in entries {
        let (from, to) = (e.path(), dst.join(e.file_name()));
        if e.file_type().map_err(io_err(&from))?.is_dir() {
            copy_tree(&from, &to)?;
        } else {
            fs::copy(&from, &to).map_err(io_err(&from))?;
        }
    }
    Ok(())
}

/// Writes JSON in the canonical form: keys sorted, 2-space indent, LF, final newline.
pub fn write_canonical(path: &Path, value: &serde_json::Value) -> Result<(), MigrateError> {
    let mut text = serde_json::to_string_pretty(value).expect("JSON value serializes");
    text.push('\n');
    fs::write(path, text).map_err(io_err(path))
}

/// Reads the project format, 0 when it is absent.
pub fn project_format(dir: &Path) -> Result<i64, MigrateError> {
    let path = dir.join("project.json");
    let text = fs::read_to_string(&path).map_err(|_| MigrateError::BadProject)?;
    let value: serde_json::Value = serde_json::from_str(&text).map_err(|_| MigrateError::BadProject)?;
    match value.get("format") {
        None => Ok(0),
        Some(v) => v.as_i64().ok_or(MigrateError::BadProject),
    }
}

/// Copies the project in `src` to the new folder `dst` and migrates the copy to the
/// current format. `src` is only read. A project already at the current format is
/// copied as it is.
pub fn migrate_to(src: &Path, dst: &Path) -> Result<MigrationReport, MigrateError> {
    let from = project_format(src)?;
    if from > FORMAT {
        return Err(MigrateError::TooNew(from));
    }
    if dst.exists() {
        return Err(MigrateError::DestinationExists(dst.to_owned()));
    }
    copy_tree(src, dst)?;
    let mut report = MigrationReport { from, to: from, removed: Vec::new() };
    while report.to < FORMAT {
        match report.to {
            0 => format_0_to_1(dst, &mut report)?,
            v => unreachable!("no migration from format {v}"),
        }
        report.to += 1;
    }
    Ok(report)
}

/// 0 → 1: engine bindings are gone (the game contract is `manifest.json`), the rest
/// of format 0 is the same as format 1.
fn format_0_to_1(dir: &Path, report: &mut MigrationReport) -> Result<(), MigrateError> {
    let bindings = dir.join(GODOT_BINDINGS);
    if bindings.is_file() {
        fs::remove_file(&bindings).map_err(io_err(&bindings))?;
        report.removed.push(GODOT_BINDINGS.to_owned());
        let engine = dir.join("engine");
        if fs::read_dir(&engine).map_err(io_err(&engine))?.next().is_none() {
            fs::remove_dir(&engine).map_err(io_err(&engine))?;
        }
    }
    let path = dir.join("project.json");
    let text = fs::read_to_string(&path).map_err(io_err(&path))?;
    let mut value: serde_json::Value = serde_json::from_str(&text).map_err(|_| MigrateError::BadProject)?;
    let obj = value.as_object_mut().ok_or(MigrateError::BadProject)?;
    obj.insert("format".into(), 1.into());
    write_canonical(&path, &value)
}

//! Validator of a `game/villagers/` folder (docs/ai/data-model.md, «Версии, миграции и ошибки»).
//!
//! [`validate_project`] reads every file, then checks references, ranges, port types,
//! graph cycles and the manifest. Each diagnostic carries a code, the file, the
//! resource id, a JSON Pointer to the field and the graph node id when there is one.
//! Diagnostics come out sorted, so two runs over the same folder print the same text.

mod check;
mod graph;
mod load;

use std::fmt;
use std::path::Path;

use serde::Serialize;

pub use load::Data;

/// Validator codes. `E_IMPL_MISSING` (the game checks it at start-up) and
/// `E_SNAPSHOT_CONTRACT` (snapshot loading, stage 4) are not produced here.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash, Serialize)]
pub enum Code {
    #[serde(rename = "E_SCHEMA")]
    Schema,
    #[serde(rename = "E_ID_FORMAT")]
    IdFormat,
    #[serde(rename = "E_ID_DUPLICATE")]
    IdDuplicate,
    #[serde(rename = "E_REF_MISSING")]
    RefMissing,
    #[serde(rename = "E_REF_KIND")]
    RefKind,
    #[serde(rename = "E_RANGE")]
    Range,
    #[serde(rename = "E_PORT_TYPE")]
    PortType,
    #[serde(rename = "E_GRAPH_CYCLE")]
    GraphCycle,
    #[serde(rename = "E_MANIFEST")]
    Manifest,
    #[serde(rename = "E_OP_LEVEL")]
    OpLevel,
    #[serde(rename = "E_SCHEDULE_GAP")]
    ScheduleGap,
    #[serde(rename = "E_LOC_MISSING")]
    LocMissing,
    #[serde(rename = "W_DEAD_BRANCH")]
    DeadBranch,
    #[serde(rename = "W_UNUSED")]
    Unused,
}

impl Code {
    pub fn as_str(self) -> &'static str {
        match self {
            Code::Schema => "E_SCHEMA",
            Code::IdFormat => "E_ID_FORMAT",
            Code::IdDuplicate => "E_ID_DUPLICATE",
            Code::RefMissing => "E_REF_MISSING",
            Code::RefKind => "E_REF_KIND",
            Code::Range => "E_RANGE",
            Code::PortType => "E_PORT_TYPE",
            Code::GraphCycle => "E_GRAPH_CYCLE",
            Code::Manifest => "E_MANIFEST",
            Code::OpLevel => "E_OP_LEVEL",
            Code::ScheduleGap => "E_SCHEDULE_GAP",
            Code::LocMissing => "E_LOC_MISSING",
            Code::DeadBranch => "W_DEAD_BRANCH",
            Code::Unused => "W_UNUSED",
        }
    }

    pub fn parse(s: &str) -> Option<Code> {
        [
            Code::Schema,
            Code::IdFormat,
            Code::IdDuplicate,
            Code::RefMissing,
            Code::RefKind,
            Code::Range,
            Code::PortType,
            Code::GraphCycle,
            Code::Manifest,
            Code::OpLevel,
            Code::ScheduleGap,
            Code::LocMissing,
            Code::DeadBranch,
            Code::Unused,
        ]
        .into_iter()
        .find(|c| c.as_str() == s)
    }

    pub fn severity(self) -> Severity {
        match self {
            Code::DeadBranch | Code::Unused => Severity::Warning,
            _ => Severity::Error,
        }
    }

    /// Localisation key of the message shown by the editor.
    pub fn message_key(self) -> String {
        format!("ai.validator.{}", self.as_str().to_ascii_lowercase())
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Severity {
    Error,
    Warning,
}

/// One validator finding, in the format of data-model.md.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Serialize)]
pub struct Diagnostic {
    /// Relative to the project folder, `/` separators: `actions/eat.json`.
    pub file: String,
    /// JSON Pointer to the field, `""` for the whole file.
    pub path: String,
    pub code: Code,
    pub severity: Severity,
    pub resource: Option<String>,
    pub node: Option<String>,
    pub message_key: String,
}

impl fmt::Display for Diagnostic {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} {} {}", self.code.as_str(), self.file, if self.path.is_empty() { "/" } else { &self.path })?;
        if let Some(node) = &self.node {
            write!(f, " node={node}")?;
        }
        Ok(())
    }
}

/// Everything the validator found, sorted by file, path and code.
#[derive(Clone, Debug, Default, Serialize)]
pub struct Report {
    pub diagnostics: Vec<Diagnostic>,
}

impl Report {
    pub fn has_errors(&self) -> bool {
        self.diagnostics.iter().any(|d| d.severity == Severity::Error)
    }

    /// One line per diagnostic, as printed by `examples/validate.rs` and kept in test expectations.
    pub fn lines(&self) -> Vec<String> {
        self.diagnostics.iter().map(|d| d.to_string()).collect()
    }
}

/// Collects diagnostics for one run.
#[derive(Default)]
pub(crate) struct Sink {
    out: Vec<Diagnostic>,
}

impl Sink {
    pub(crate) fn push(&mut self, code: Code, file: &str, resource: Option<&str>, path: &str, node: Option<&str>) {
        self.out.push(Diagnostic {
            file: file.to_owned(),
            path: path.to_owned(),
            code,
            severity: code.severity(),
            resource: resource.map(str::to_owned),
            node: node.map(str::to_owned),
            message_key: code.message_key(),
        });
    }

    pub(crate) fn len(&self) -> usize {
        self.out.len()
    }

    fn finish(mut self) -> Report {
        self.out.sort();
        self.out.dedup();
        Report { diagnostics: self.out }
    }
}

/// Appends one JSON Pointer segment, escaping `~` and `/`.
pub(crate) fn ptr(base: &str, seg: impl fmt::Display) -> String {
    let seg = seg.to_string().replace('~', "~0").replace('/', "~1");
    format!("{base}/{seg}")
}

/// Validates the project folder `dir` (the one that holds `project.json`).
pub fn validate_project(dir: &Path) -> Report {
    let mut sink = Sink::default();
    let data = load::load(dir, &mut sink);
    check::run(&data, &mut sink);
    sink.finish()
}

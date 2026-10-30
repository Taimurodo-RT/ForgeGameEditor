//! Reading the project folder into typed data. Every file that fails to parse gives
//! one diagnostic (serde stops at the first error) and is left out of later checks.

use std::collections::BTreeMap;
use std::fs;
use std::path::Path;

use serde::de::DeserializeOwned;

use super::{Code, Sink, ptr};
use crate::data::*;

/// A parsed file and where it came from.
pub struct Res<T> {
    pub file: String,
    pub value: T,
}

/// All files of a project that parsed. Maps are keyed by the id the file name gives
/// (`action.eat` for `actions/eat.json`), so iteration order is fixed.
#[derive(Default)]
pub struct Data {
    pub project: Option<Res<Project>>,
    pub manifest: Option<Res<Manifest>>,
    /// Language → key → text, only for the languages that have a file.
    pub loc: BTreeMap<String, BTreeMap<String, String>>,
    pub needs: BTreeMap<String, Res<Need>>,
    pub traits: BTreeMap<String, Res<Trait>>,
    pub archetypes: BTreeMap<String, Res<Archetype>>,
    pub curves: BTreeMap<String, Res<Curve>>,
    pub objects: BTreeMap<String, Res<Object>>,
    pub actions: BTreeMap<String, Res<Action>>,
    pub jobs: BTreeMap<String, Res<Job>>,
    pub roles: BTreeMap<String, Res<Role>>,
    pub stimuli: BTreeMap<String, Res<Stimulus>>,
    pub emotions: BTreeMap<String, Res<Emotion>>,
    pub reactions: BTreeMap<String, Res<Reaction>>,
    pub events: BTreeMap<String, Res<Event>>,
    pub schedules: BTreeMap<String, Res<Schedule>>,
    pub graphs: BTreeMap<String, Res<Graph>>,
    pub subgraphs: BTreeMap<String, Res<Graph>>,
    pub scenarios: BTreeMap<String, Res<Scenario>>,
    /// Ids of every file found, parsed or not, so a broken file does not also show
    /// up as a missing reference everywhere it is used.
    pub ids: BTreeMap<Kind, Vec<String>>,
    /// Some file did not parse: whether a resource is unused cannot be known.
    pub incomplete: bool,
}

impl Data {
    pub fn exists(&self, kind: Kind, id: &str) -> bool {
        self.ids.get(&kind).is_some_and(|v| v.binary_search_by(|x| x.as_str().cmp(id)).is_ok())
    }

    pub fn graph(&self, id: &str) -> Option<&Res<Graph>> {
        self.graphs.get(id).or_else(|| self.subgraphs.get(id))
    }
}

/// Splits a serde error into the validator code and the JSON Pointer to the field.
pub(crate) fn split_error(err: serde_path_to_error::Error<serde_json::Error>, base: &str) -> (Code, String) {
    use serde_path_to_error::Segment;
    let mut path = base.to_owned();
    for seg in err.path().iter() {
        match seg {
            Segment::Seq { index } => path = ptr(&path, index),
            Segment::Map { key } => path = ptr(&path, key),
            Segment::Enum { variant } => path = ptr(&path, variant),
            Segment::Unknown => {}
        }
    }
    let text = err.into_inner().to_string();
    let code = text.split_once('|').and_then(|(c, _)| Code::parse(c)).unwrap_or(Code::Schema);
    (code, path)
}

/// Deserializes a JSON value, reporting the first error with its pointer under `base`.
pub(crate) fn parse_value<T: DeserializeOwned>(
    value: serde_json::Value,
    sink: &mut Sink,
    file: &str,
    resource: Option<&str>,
    base: &str,
    node: Option<&str>,
) -> Option<T> {
    match serde_path_to_error::deserialize::<_, T>(value) {
        Ok(v) => Some(v),
        Err(err) => {
            let (code, path) = split_error(err, base);
            sink.push(code, file, resource, &path, node);
            None
        }
    }
}

fn read_json(dir: &Path, file: &str, sink: &mut Sink, resource: Option<&str>) -> Option<serde_json::Value> {
    let text = match fs::read_to_string(dir.join(file)) {
        Ok(t) => t,
        Err(_) => {
            sink.push(Code::Schema, file, resource, "", None);
            return None;
        }
    };
    match serde_json::from_str(&text) {
        Ok(v) => Some(v),
        Err(_) => {
            sink.push(Code::Schema, file, resource, "", None);
            None
        }
    }
}

fn read_file<T: DeserializeOwned>(dir: &Path, file: &str, sink: &mut Sink, resource: Option<&str>) -> Option<T> {
    let value = read_json(dir, file, sink, resource)?;
    parse_value(value, sink, file, resource, "", None)
}

/// File names in a folder, sorted; an absent folder is empty.
fn list(dir: &Path, folder: &str) -> Vec<String> {
    let mut names: Vec<String> = fs::read_dir(dir.join(folder))
        .map(|rd| {
            rd.filter_map(Result::ok)
                .filter(|e| e.file_type().is_ok_and(|t| t.is_file()))
                .filter_map(|e| e.file_name().into_string().ok())
                .collect()
        })
        .unwrap_or_default();
    names.sort();
    names
}

fn load_kind<T: DeserializeOwned + Header>(
    dir: &Path,
    kind: Kind,
    sink: &mut Sink,
    ids: &mut BTreeMap<Kind, Vec<String>>,
) -> BTreeMap<String, Res<T>> {
    let mut out = BTreeMap::new();
    for name in list(dir, kind.folder()) {
        let file = format!("{}/{}", kind.folder(), name);
        let Some(stem) = name.strip_suffix(".json") else {
            sink.push(Code::Schema, &file, None, "", None);
            continue;
        };
        let id = format!("{}.{}", kind.prefix(), stem);
        if !is_id(&id) {
            sink.push(Code::IdFormat, &file, None, "", None);
            continue;
        }
        ids.entry(kind).or_default().push(id.clone());
        let Some(value) = read_file::<T>(dir, &file, sink, Some(&id)) else {
            continue;
        };
        if value.id().as_str() != id {
            sink.push(Code::IdFormat, &file, Some(&id), "/id", None);
        }
        if value.schema() != kind.schema_tag() {
            sink.push(Code::Schema, &file, Some(&id), "/schema", None);
        }
        out.insert(id, Res { file, value });
    }
    out
}

pub(crate) fn load(dir: &Path, sink: &mut Sink) -> Data {
    let before = sink.len();
    let mut data = Data::default();
    if dir.join("project.json").is_file() {
        data.project =
            read_file(dir, "project.json", sink, None).map(|value| Res { file: "project.json".into(), value });
    } else {
        sink.push(Code::Schema, "project.json", None, "", None);
    }
    if dir.join("manifest.json").is_file() {
        data.manifest =
            read_file(dir, "manifest.json", sink, None).map(|value| Res { file: "manifest.json".into(), value });
    } else {
        sink.push(Code::Manifest, "manifest.json", None, "", None);
    }
    for name in list(dir, "loc") {
        let file = format!("loc/{name}");
        let Some(lang) = name.strip_suffix(".json") else {
            sink.push(Code::Schema, &file, None, "", None);
            continue;
        };
        if let Some(map) = read_file::<BTreeMap<String, String>>(dir, &file, sink, None) {
            data.loc.insert(lang.to_owned(), map);
        }
    }

    let ids = &mut data.ids;
    data.needs = load_kind(dir, Kind::Need, sink, ids);
    data.traits = load_kind(dir, Kind::Trait, sink, ids);
    data.archetypes = load_kind(dir, Kind::Archetype, sink, ids);
    data.curves = load_kind(dir, Kind::Curve, sink, ids);
    data.objects = load_kind(dir, Kind::Object, sink, ids);
    data.actions = load_kind(dir, Kind::Action, sink, ids);
    data.jobs = load_kind(dir, Kind::Job, sink, ids);
    data.roles = load_kind(dir, Kind::Role, sink, ids);
    data.stimuli = load_kind(dir, Kind::Stimulus, sink, ids);
    data.emotions = load_kind(dir, Kind::Emotion, sink, ids);
    data.reactions = load_kind(dir, Kind::Reaction, sink, ids);
    data.events = load_kind(dir, Kind::Event, sink, ids);
    data.schedules = load_kind(dir, Kind::Schedule, sink, ids);
    data.graphs = load_kind(dir, Kind::Graph, sink, ids);
    data.subgraphs = load_kind(dir, Kind::Subgraph, sink, ids);
    data.scenarios = load_kind(dir, Kind::Scenario, sink, ids);
    // The dialogue editor comes later: validator v1 accepts only an empty folder.
    for name in list(dir, Kind::Dialogue.folder()) {
        sink.push(Code::Schema, &format!("{}/{}", Kind::Dialogue.folder(), name), None, "", None);
    }
    for v in data.ids.values_mut() {
        v.sort();
    }
    data.incomplete = sink.len() > before;
    data
}

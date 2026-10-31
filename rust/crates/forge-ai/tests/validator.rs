//! The validator on purposely broken projects. Each case copies `tests/data/valid`,
//! breaks one thing and must give exactly the listed diagnostics, no more and no less.

use std::fs;
use std::path::{Path, PathBuf};

use forge_ai::validate::validate_project;
use serde_json::{Value, json};

const VALID: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/tests/data/valid");

enum Edit {
    /// Replace or add the value at a JSON Pointer (`/-` appends to an array).
    Set(&'static str, &'static str, Value),
    /// Remove the value at a JSON Pointer.
    Remove(&'static str, &'static str),
    /// Write a file as raw text.
    Write(&'static str, &'static str),
}

fn copy_tree(src: &Path, dst: &Path) {
    fs::create_dir_all(dst).unwrap();
    for e in fs::read_dir(src).unwrap() {
        let e = e.unwrap();
        let to = dst.join(e.file_name());
        if e.file_type().unwrap().is_dir() {
            copy_tree(&e.path(), &to);
        } else {
            fs::copy(e.path(), to).unwrap();
        }
    }
}

fn parent_and_key(ptr: &str) -> (&str, &str) {
    ptr.rsplit_once('/').unwrap()
}

fn apply(dir: &Path, edit: &Edit) {
    let edit_json = |file: &str, f: &dyn Fn(&mut Value)| {
        let path = dir.join(file);
        let mut v: Value = serde_json::from_str(&fs::read_to_string(&path).unwrap()).unwrap();
        f(&mut v);
        fs::write(&path, serde_json::to_string_pretty(&v).unwrap()).unwrap();
    };
    match edit {
        Edit::Set(file, ptr, value) => edit_json(file, &|v| {
            let (parent, key) = parent_and_key(ptr);
            match v.pointer_mut(parent).unwrap() {
                Value::Object(m) => {
                    m.insert(key.to_owned(), value.clone());
                }
                Value::Array(a) if key == "-" => a.push(value.clone()),
                Value::Array(a) => a[key.parse::<usize>().unwrap()] = value.clone(),
                _ => panic!("{ptr}: not a container"),
            }
        }),
        Edit::Remove(file, ptr) => edit_json(file, &|v| {
            let (parent, key) = parent_and_key(ptr);
            match v.pointer_mut(parent).unwrap() {
                Value::Object(m) => {
                    m.remove(key).unwrap();
                }
                Value::Array(a) => {
                    a.remove(key.parse::<usize>().unwrap());
                }
                _ => panic!("{ptr}: not a container"),
            }
        }),
        Edit::Write(file, text) => {
            let path = dir.join(file);
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(path, text).unwrap();
        }
    }
}

fn broken(name: &str, edits: &[Edit]) -> PathBuf {
    let dir = Path::new(env!("CARGO_TARGET_TMPDIR")).join("validator").join(name);
    let _ = fs::remove_dir_all(&dir);
    copy_tree(Path::new(VALID), &dir);
    for e in edits {
        apply(&dir, e);
    }
    dir
}

fn check(name: &str, edits: &[Edit], expected: &[&str]) {
    let dir = broken(name, edits);
    let got = validate_project(&dir).lines();
    assert_eq!(got, expected, "case `{name}`");
}

use Edit::*;

#[test]
fn valid_project_is_clean() {
    let report = validate_project(Path::new(VALID));
    assert_eq!(report.lines(), Vec::<String>::new());
}

#[test]
fn schema_errors() {
    check("unknown_field", &[Set("actions/eat.json", "/colour", json!("red"))], &["E_SCHEMA actions/eat.json /colour"]);
    // A file that does not load turns off `W_UNUSED`: its references are unknown.
    check("missing_field", &[Remove("actions/eat.json", "/graph")], &["E_SCHEMA actions/eat.json /"]);
    check("bad_json", &[Write("needs/food.json", "{")], &["E_SCHEMA needs/food.json /"]);
    check("wrong_type", &[Set("needs/food.json", "/start", json!("full"))], &["E_SCHEMA needs/food.json /start"]);
    check(
        "schema_version",
        &[Set("needs/food.json", "/schema", json!("need/2"))],
        &["E_SCHEMA needs/food.json /schema"],
    );
    check("old_format", &[Set("project.json", "/format", json!(0))], &["E_SCHEMA project.json /format"]);
    check("dialogue", &[Write("dialogues/hello.json", "{}")], &["E_SCHEMA dialogues/hello.json /"]);
    check(
        "life_without_schedule",
        &[Remove("archetypes/worker.json", "/schedule")],
        &["E_SCHEMA archetypes/worker.json /schedule"],
    );
    check(
        "unknown_op",
        &[Set("graphs/action_eat.json", "/nodes/4/op", json!("pow"))],
        &["E_SCHEMA graphs/action_eat.json /nodes/4/op node=product"],
    );
    check(
        "curve_without_points",
        &[Remove("curves/hunger.json", "/params/points")],
        &["E_SCHEMA curves/hunger.json /params/points"],
    );
}

#[test]
fn id_errors() {
    check(
        "id_mismatch",
        &[Set("actions/eat.json", "/id", json!("action.drink"))],
        &["E_ID_FORMAT actions/eat.json /id"],
    );
    check(
        "bad_loc_key",
        &[Set("curves/hunger.json", "/name", json!("hunger"))],
        &["E_ID_FORMAT curves/hunger.json /name"],
    );
    check(
        "duplicate_slot",
        &[Set("objects/storehouse.json", "/slots/-", json!({"id": "door", "offset": [0, 1], "facing": "n"}))],
        &["E_ID_DUPLICATE objects/storehouse.json /slots/1/id"],
    );
}

#[test]
fn reference_errors() {
    check(
        "ref_missing",
        &[Set("needs/food.json", "/urgency", json!("curve.nope"))],
        &["E_REF_MISSING needs/food.json /urgency"],
    );
    check(
        "ref_kind",
        &[Set("needs/food.json", "/urgency", json!("need.food"))],
        &["E_REF_KIND needs/food.json /urgency"],
    );
    check(
        "unknown_tag",
        &[Set("objects/storehouse.json", "/tags", json!(["metal"]))],
        &["E_REF_MISSING objects/storehouse.json /tags/0"],
    );
    check(
        "unknown_bucket",
        &[Set("actions/eat.json", "/bucket/default", json!("leisure"))],
        &["E_REF_MISSING actions/eat.json /bucket/default"],
    );
    check(
        "unknown_phase",
        &[Set(
            "needs/food.json",
            "/decay_modifiers/0/when",
            json!({"event_phase_is": {"event": "event.festival", "phase": "dance"}}),
        )],
        &["E_REF_MISSING needs/food.json /decay_modifiers/0/when/event_phase_is/phase"],
    );
    check(
        "graph_of_wrong_kind",
        &[Set("actions/eat.json", "/graph", json!("graph.fire_strength"))],
        &["E_REF_KIND actions/eat.json /graph", "W_UNUSED graphs/action_eat.json /"],
    );
}

#[test]
fn range_errors() {
    check("unit_above_one", &[Set("project.json", "/stay_ratio", json!(1.5))], &["E_RANGE project.json /stay_ratio"]);
    check("five_decimals", &[Set("needs/food.json", "/start", json!(80.00001))], &["E_RANGE needs/food.json /start"]);
    check(
        "six_buckets",
        &[Set(
            "project.json",
            "/buckets",
            json!(
                ["survival", "crisis", "c", "d", "e", "life"]
                    .map(|id| json!({"id": id, "threshold": 0, "interrupt": "never"}))
            ),
        )],
        &["E_RANGE project.json /buckets"],
    );
    check(
        "points_not_ascending",
        &[Set("curves/hunger.json", "/params/points/1", json!([0, 0.2]))],
        &["E_RANGE curves/hunger.json /params/points/1"],
    );
    check(
        "bad_time",
        &[Set("schedules/worker.json", "/blocks/0/from", json!("25:00"))],
        &["E_RANGE schedules/worker.json /blocks/0/from"],
    );
}

#[test]
fn graph_errors() {
    check(
        "port_type",
        &[Set("graphs/action_eat.json", "/nodes/2", json!({"id": "light", "op": "const", "params": {"value": 0.5}}))],
        &["E_PORT_TYPE graphs/action_eat.json /edges/2 node=fix"],
    );
    check(
        "unknown_port",
        &[Set("graphs/action_eat.json", "/edges/0/to", json!("urgency.input"))],
        &["E_PORT_TYPE graphs/action_eat.json /edges/0/to node=urgency"],
    );
    check(
        "two_links_into_input",
        &[Set("graphs/action_eat.json", "/edges/-", json!({"from": "light.value", "to": "product.a"}))],
        &["E_PORT_TYPE graphs/action_eat.json /edges/4/to node=product"],
    );
    check(
        "wrong_output",
        &[Set("graphs/action_eat.json", "/outputs", json!({"utility": "product.out"}))],
        &["E_SCHEMA graphs/action_eat.json /outputs"],
    );
    check(
        "cycle",
        &[
            Set("subgraphs/dim.json", "/nodes/-", json!({"id": "m1", "op": "mul"})),
            Set("subgraphs/dim.json", "/nodes/-", json!({"id": "m2", "op": "mul"})),
            Set("subgraphs/dim.json", "/edges/-", json!({"from": "m1.out", "to": "m2.a"})),
            Set("subgraphs/dim.json", "/edges/-", json!({"from": "m2.out", "to": "m1.a"})),
        ],
        &[
            "E_GRAPH_CYCLE subgraphs/dim.json /nodes/2 node=m1",
            "W_DEAD_BRANCH subgraphs/dim.json /nodes/2 node=m1",
            "W_DEAD_BRANCH subgraphs/dim.json /nodes/3 node=m2",
        ],
    );
    check(
        "subgraph_calls_itself",
        &[
            Set(
                "subgraphs/dim.json",
                "/nodes/-",
                json!({"id": "again", "op": "subgraph", "params": {"subgraph": "subgraph.dim"}}),
            ),
            Set("subgraphs/dim.json", "/edges/-", json!({"from": "x.value", "to": "again.x"})),
            Set("subgraphs/dim.json", "/outputs/twice", json!("again.out")),
        ],
        &["E_GRAPH_CYCLE subgraphs/dim.json /nodes/2/params/subgraph node=again"],
    );
    check(
        "dead_branch",
        &[Set("graphs/action_eat.json", "/nodes/-", json!({"id": "spare", "op": "const", "params": {"value": 1}}))],
        &["W_DEAD_BRANCH graphs/action_eat.json /nodes/5 node=spare"],
    );
    check(
        "flow_in_utility",
        &[Set("graphs/action_eat.json", "/nodes/-", json!({"id": "pause", "op": "wait", "params": {"minutes": 5}}))],
        &["E_SCHEMA graphs/action_eat.json /nodes/5/op node=pause"],
    );
    check(
        "bad_param",
        &[Set("subgraphs/dim.json", "/nodes/1/params/n", json!(40))],
        &["E_RANGE subgraphs/dim.json /nodes/1/params/n node=comp"],
    );
}

#[test]
fn manifest_errors() {
    check(
        "unknown_step",
        &[Set("actions/eat.json", "/steps/0/type", json!("fly"))],
        &["E_MANIFEST actions/eat.json /steps/0/type"],
    );
    check(
        "unknown_arg",
        &[Set("actions/eat.json", "/steps/1/args/colour", json!("red"))],
        &["E_MANIFEST actions/eat.json /steps/1/args/colour"],
    );
    check(
        "missing_arg",
        &[Remove("actions/eat.json", "/steps/1/args/count")],
        &["E_MANIFEST actions/eat.json /steps/1/args"],
    );
    check(
        "arg_type",
        &[Set("actions/eat.json", "/steps/1/args/count", json!("one"))],
        &["E_MANIFEST actions/eat.json /steps/1/args/count"],
    );
    check(
        "unknown_animation",
        &[Set("actions/eat.json", "/steps/2/args/animation", json!("dance"))],
        &["E_MANIFEST actions/eat.json /steps/2/args/animation"],
    );
    check(
        "unknown_channel",
        &[Set("stimuli/fire.json", "/channels/0", json!("smell"))],
        &["E_MANIFEST stimuli/fire.json /channels/0"],
    );
    check(
        "unknown_input",
        &[Set("graphs/action_eat.json", "/nodes/2/params/input", json!("rain"))],
        &["E_MANIFEST graphs/action_eat.json /nodes/2/params/input node=light"],
    );
    check("manifest_format", &[Set("manifest.json", "/format", json!(2))], &["E_RANGE manifest.json /format"]);
}

#[test]
fn schedule_loc_and_unused() {
    check(
        "schedule_gap",
        &[Set("schedules/worker.json", "/blocks/2/to", json!("21:00"))],
        &["E_SCHEDULE_GAP schedules/worker.json /blocks"],
    );
    check(
        "schedule_overlap",
        &[Set("schedules/worker.json", "/blocks/2/to", json!("23:00"))],
        &["E_SCHEDULE_GAP schedules/worker.json /blocks"],
    );
    check("loc_missing", &[Remove("loc/ru.json", "/loc.need.food.name")], &["E_LOC_MISSING needs/food.json /name"]);
    check(
        "unused",
        &[Write(
            "stimuli/smoke.json",
            r#"{"id": "stimulus.smoke", "schema": "stimulus/1", "name": "loc.stimulus.fire.name", "tags": [],
                "channels": ["sight"], "radius": 4, "duration": 10, "interrupts": false, "strength": 0.2}"#,
        )],
        &["W_UNUSED stimuli/smoke.json /"],
    );
}

#[test]
fn report_is_sorted_and_repeatable() {
    let dir = broken(
        "many",
        &[
            Set("needs/food.json", "/urgency", json!("curve.nope")),
            Set("actions/eat.json", "/steps/0/type", json!("fly")),
            Set("project.json", "/stay_ratio", json!(2)),
        ],
    );
    let a = validate_project(&dir);
    let b = validate_project(&dir);
    assert_eq!(a.lines(), b.lines());
    assert!(a.diagnostics.is_sorted());
    assert_eq!(a.diagnostics.len(), 3);
}

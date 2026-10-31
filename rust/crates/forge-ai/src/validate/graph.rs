//! Graph checks: operation parameters, port types of links, outputs, cycles inside a
//! graph and through subgraph calls, nodes that reach no output.

use std::collections::{BTreeMap, BTreeSet};

use serde::Deserialize;
use serde::de::DeserializeOwned;

use super::check::{At, Ctx};
use super::load::{Res, split_error};
use super::{Code, ptr};
use crate::data::*;

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct InputP {
    input: LocalId,
    #[serde(default)]
    args: BTreeMap<String, serde_json::Value>,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct NeedP {
    need: Ref,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct ConstP {
    #[allow(dead_code)] // range is checked while parsing
    value: Real,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct CurveP {
    curve: Ref,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct NoP {}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct CompensateP {
    #[allow(dead_code)]
    n: Int<1, 32>,
}
#[derive(Deserialize)]
#[serde(rename_all = "snake_case")]
enum Cmp {
    Lt,
    Le,
    Gt,
    Ge,
    Eq,
    Ne,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct CompareP {
    #[allow(dead_code)]
    cmp: Cmp,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct SubgraphP {
    subgraph: Ref,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct SubgraphInputP {
    input: LocalId,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct WaitP {
    #[allow(dead_code)]
    minutes: Minutes,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct EmitP {
    stimulus: Ref,
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct StartActionP {
    action: Ref,
    bucket: LocalId,
}

/// Parsed parameters of a node, by operation.
enum Params {
    Input(InputP),
    Need(NeedP),
    Const,
    Curve(CurveP),
    Unit2,
    Real2,
    Lerp,
    Compensate,
    Compare,
    Bool2,
    Not,
    Select,
    Subgraph(SubgraphP),
    SubgraphInput(SubgraphInputP),
    Sequence,
    Wait,
    Emit(EmitP),
    StartAction(StartActionP),
}

impl Params {
    fn is_flow(&self) -> bool {
        matches!(self, Params::Sequence | Params::Wait | Params::Emit(_) | Params::StartAction(_))
    }
}

type ParseErr = (Code, String);

fn parse<T: DeserializeOwned>(params: &serde_json::Map<String, serde_json::Value>, base: &str) -> Result<T, ParseErr> {
    serde_path_to_error::deserialize(serde_json::Value::Object(params.clone())).map_err(|e| split_error(e, base))
}

/// Parses `node.params` for its operation. `base` is the pointer to the params.
fn parse_params(node: &Node, base: &str) -> Result<Params, ParseErr> {
    let p = &node.params;
    Ok(match node.op.as_str() {
        "input" => Params::Input(parse(p, base)?),
        "need" => Params::Need(parse(p, base)?),
        "const" => parse::<ConstP>(p, base).map(|_| Params::Const)?,
        "curve" => Params::Curve(parse(p, base)?),
        "mul" | "min" | "max" => parse::<NoP>(p, base).map(|_| Params::Unit2)?,
        "add" => parse::<NoP>(p, base).map(|_| Params::Real2)?,
        "lerp" => parse::<NoP>(p, base).map(|_| Params::Lerp)?,
        "compensate" => parse::<CompensateP>(p, base).map(|_| Params::Compensate)?,
        "compare" => parse::<CompareP>(p, base).map(|_| Params::Compare)?,
        "and" | "or" => parse::<NoP>(p, base).map(|_| Params::Bool2)?,
        "not" => parse::<NoP>(p, base).map(|_| Params::Not)?,
        "select" => parse::<NoP>(p, base).map(|_| Params::Select)?,
        "subgraph" => Params::Subgraph(parse(p, base)?),
        "subgraph_input" => Params::SubgraphInput(parse(p, base)?),
        "sequence" => parse::<NoP>(p, base).map(|_| Params::Sequence)?,
        "wait" => parse::<WaitP>(p, base).map(|_| Params::Wait)?,
        "emit" => Params::Emit(parse(p, base)?),
        "start_action" => Params::StartAction(parse(p, base)?),
        _ => return Err((Code::Schema, base.trim_end_matches("/params").to_owned() + "/op")),
    })
}

/// Ports of a node; a type of `None` is unknown (a broken reference) and is not checked.
#[derive(Default)]
struct Sig {
    inputs: Vec<(String, Option<PortType>)>,
    outputs: Vec<(String, Option<PortType>)>,
}

fn ports(list: &[(&str, PortType)]) -> Vec<(String, Option<PortType>)> {
    list.iter().map(|(n, t)| (n.to_string(), Some(*t))).collect()
}

/// Output types of a subgraph by name, following nested subgraphs; `stack` stops recursion.
fn subgraph_outputs(ctx: &Ctx, id: &str, stack: &mut Vec<String>) -> BTreeMap<String, Option<PortType>> {
    let Some(g) = ctx.data.subgraphs.get(id) else { return BTreeMap::new() };
    if stack.iter().any(|s| s == id) {
        return g.value.outputs.keys().map(|k| (k.clone(), None)).collect();
    }
    stack.push(id.to_owned());
    let out = g
        .value
        .outputs
        .iter()
        .map(|(name, target)| {
            let ty = split_port(target).and_then(|(n, p)| {
                let node = g.value.nodes.iter().find(|x| x.id.as_str() == n)?;
                let sig = node_sig(ctx, &g.value, node, stack);
                sig.outputs.into_iter().find(|(q, _)| q == p).and_then(|(_, t)| t)
            });
            (name.clone(), ty)
        })
        .collect();
    stack.pop();
    out
}

fn node_sig(ctx: &Ctx, graph: &Graph, node: &Node, stack: &mut Vec<String>) -> Sig {
    use PortType::*;
    let Ok(params) = parse_params(node, "") else { return Sig::default() };
    match params {
        Params::Input(p) => {
            let ty = ctx.manifest().and_then(|m| m.inputs.iter().find(|i| i.id == p.input)).map(|i| i.type_);
            Sig { inputs: vec![], outputs: vec![("value".into(), ty)] }
        }
        Params::Need(_) => Sig { inputs: vec![], outputs: ports(&[("value", Unit)]) },
        Params::Const => Sig { inputs: vec![], outputs: ports(&[("value", Real)]) },
        Params::Curve(_) => Sig { inputs: ports(&[("x", Real)]), outputs: ports(&[("y", Unit)]) },
        Params::Unit2 => Sig { inputs: ports(&[("a", Unit), ("b", Unit)]), outputs: ports(&[("out", Unit)]) },
        Params::Real2 => Sig { inputs: ports(&[("a", Real), ("b", Real)]), outputs: ports(&[("out", Real)]) },
        Params::Lerp => {
            Sig { inputs: ports(&[("a", Unit), ("b", Unit), ("t", Unit)]), outputs: ports(&[("out", Unit)]) }
        }
        Params::Compensate => Sig { inputs: ports(&[("x", Unit)]), outputs: ports(&[("out", Unit)]) },
        Params::Compare => Sig { inputs: ports(&[("a", Real), ("b", Real)]), outputs: ports(&[("out", Bool)]) },
        Params::Bool2 => Sig { inputs: ports(&[("a", Bool), ("b", Bool)]), outputs: ports(&[("out", Bool)]) },
        Params::Not => Sig { inputs: ports(&[("a", Bool)]), outputs: ports(&[("out", Bool)]) },
        Params::Select => {
            Sig { inputs: ports(&[("cond", Bool), ("a", Unit), ("b", Unit)]), outputs: ports(&[("out", Unit)]) }
        }
        Params::Subgraph(p) => {
            let inputs = ctx
                .data
                .subgraphs
                .get(p.subgraph.as_str())
                .map(|g| g.value.inputs.iter().map(|i| (i.id.0.clone(), Some(i.type_))).collect())
                .unwrap_or_default();
            let outputs = subgraph_outputs(ctx, p.subgraph.as_str(), stack).into_iter().collect();
            Sig { inputs, outputs }
        }
        Params::SubgraphInput(p) => {
            let ty = graph.inputs.iter().find(|i| i.id == p.input).map(|i| i.type_);
            Sig { inputs: vec![], outputs: vec![("value".into(), ty)] }
        }
        Params::Sequence => Sig { inputs: ports(&[("in", Flow)]), outputs: ports(&[("first", Flow), ("then", Flow)]) },
        Params::Wait | Params::Emit(_) => Sig { inputs: ports(&[("in", Flow)]), outputs: ports(&[("out", Flow)]) },
        Params::StartAction(_) => Sig { inputs: ports(&[("in", Flow)]), outputs: vec![] },
    }
}

/// `node.port` → (`node`, `port`).
fn split_port(s: &str) -> Option<(&str, &str)> {
    let (n, p) = s.split_once('.')?;
    (is_local_id(n) && is_local_id(p)).then_some((n, p))
}

/// Subgraphs a graph calls directly.
fn calls(graph: &Graph) -> Vec<String> {
    graph
        .nodes
        .iter()
        .filter(|n| n.op.as_str() == "subgraph")
        .filter_map(|n| n.params.get("subgraph").and_then(|v| v.as_str()).map(str::to_owned))
        .collect()
}

/// Whether `from` reaches `target` through subgraph calls.
fn reaches(ctx: &Ctx, from: &str, target: &str) -> bool {
    let mut seen = BTreeSet::new();
    let mut todo = vec![from.to_owned()];
    while let Some(id) = todo.pop() {
        if id == target {
            return true;
        }
        if !seen.insert(id.clone()) {
            continue;
        }
        if let Some(g) = ctx.data.subgraphs.get(&id) {
            todo.extend(calls(&g.value));
        }
    }
    false
}

pub(crate) fn check(ctx: &mut Ctx, id: &str, r: &Res<Graph>) {
    let at = At { file: &r.file, res: Some(id) };
    let g = &r.value;
    let in_subgraphs = id.starts_with("subgraph.");
    let kind_ok = if in_subgraphs { g.kind == GraphKind::Subgraph } else { g.kind != GraphKind::Subgraph };
    if !kind_ok {
        ctx.err(Code::Schema, at, "/kind");
    }
    if g.kind != GraphKind::Subgraph && !g.inputs.is_empty() {
        ctx.err(Code::Schema, at, "/inputs");
    }
    if g.nodes.len() > 256 {
        ctx.err(Code::Range, at, "/nodes");
    }
    ctx.unique(g.nodes.iter().map(|n| n.id.as_str()), at, "/nodes", "id");
    ctx.unique(g.inputs.iter().map(|i| i.id.as_str()), at, "/inputs", "id");

    // Parameters, references and per-node rules.
    let mut sigs: BTreeMap<&str, Sig> = BTreeMap::new();
    let mut flow_nodes = BTreeSet::new();
    let mut all_parsed = true;
    for (i, node) in g.nodes.iter().enumerate() {
        let nid = node.id.as_str();
        let base = ptr(&ptr("/nodes", i), "params");
        let params = match parse_params(node, &base) {
            Ok(p) => p,
            Err((code, path)) => {
                ctx.err_node(code, at, &path, nid);
                all_parsed = false;
                continue;
            }
        };
        if params.is_flow() {
            flow_nodes.insert(nid);
            if g.kind == GraphKind::Utility {
                ctx.err_node(Code::Schema, at, &ptr(&ptr("/nodes", i), "op"), nid);
            }
        }
        match &params {
            Params::Input(p) => {
                if let Some(m) = ctx.manifest() {
                    match m.inputs.iter().find(|x| x.id == p.input) {
                        Some(sig) => ctx.args(&p.args, &sig.args, at, &ptr(&base, "args"), Some(nid)),
                        None => ctx.err_node(Code::Manifest, at, &ptr(&base, "input"), nid),
                    }
                }
            }
            Params::Need(p) => node_ref(ctx, &p.need, Kind::Need, at, &ptr(&base, "need"), nid),
            Params::Curve(p) => node_ref(ctx, &p.curve, Kind::Curve, at, &ptr(&base, "curve"), nid),
            Params::Subgraph(p) => {
                let path = ptr(&base, "subgraph");
                node_ref(ctx, &p.subgraph, Kind::Subgraph, at, &path, nid);
                if ctx.data.subgraphs.contains_key(p.subgraph.as_str()) && reaches(ctx, p.subgraph.as_str(), id) {
                    ctx.err_node(Code::GraphCycle, at, &path, nid);
                }
            }
            Params::SubgraphInput(p) => {
                if g.kind != GraphKind::Subgraph {
                    ctx.err_node(Code::Schema, at, &ptr(&ptr("/nodes", i), "op"), nid);
                } else if !g.inputs.iter().any(|x| x.id == p.input) {
                    ctx.err_node(Code::RefMissing, at, &ptr(&base, "input"), nid);
                }
            }
            Params::Emit(p) => node_ref(ctx, &p.stimulus, Kind::Stimulus, at, &ptr(&base, "stimulus"), nid),
            Params::StartAction(p) => {
                node_ref(ctx, &p.action, Kind::Action, at, &ptr(&base, "action"), nid);
                if !ctx.data.project.as_ref().is_some_and(|pr| pr.value.buckets.iter().any(|b| b.id == p.bucket)) {
                    ctx.err_node(Code::RefMissing, at, &ptr(&base, "bucket"), nid);
                }
            }
            _ => {}
        }
        let mut stack = vec![id.to_owned()];
        sigs.entry(nid).or_insert_with(|| node_sig(ctx, g, node, &mut stack));
    }

    // Links: existing nodes and ports, compatible types, one link per input.
    let mut linked_inputs = BTreeSet::new();
    let mut links: Vec<(&str, &str)> = Vec::new();
    for (i, e) in g.edges.iter().enumerate() {
        let base = ptr("/edges", i);
        let (Some((fn_, fp)), Some((tn, tp))) = (split_port(&e.from), split_port(&e.to)) else {
            let bad = if split_port(&e.from).is_none() { "from" } else { "to" };
            ctx.err(Code::Schema, at, &ptr(&base, bad));
            continue;
        };
        let (Some(fs), Some(ts)) = (sigs.get(fn_), sigs.get(tn)) else {
            let bad = if sigs.contains_key(fn_) { "to" } else { "from" };
            if !g.nodes.iter().any(|n| n.id.as_str() == if bad == "to" { tn } else { fn_ }) {
                ctx.err(Code::RefMissing, at, &ptr(&base, bad));
            }
            continue;
        };
        let from_ty = fs.outputs.iter().find(|(n, _)| n == fp).map(|(_, t)| *t);
        let to_ty = ts.inputs.iter().find(|(n, _)| n == tp).map(|(_, t)| *t);
        match (from_ty, to_ty) {
            (None, _) => ctx.err_node(Code::PortType, at, &ptr(&base, "from"), fn_),
            (_, None) => ctx.err_node(Code::PortType, at, &ptr(&base, "to"), tn),
            (Some(Some(a)), Some(Some(b))) if !a.connects_to(b) => ctx.err_node(Code::PortType, at, &base, tn),
            _ => {}
        }
        if !linked_inputs.insert((tn, tp)) {
            ctx.err_node(Code::PortType, at, &ptr(&base, "to"), tn);
        }
        links.push((fn_, tn));
    }

    // Outputs of the graph.
    let required: &[(&str, PortType)] = match g.kind {
        GraphKind::Utility => &[("score", PortType::Unit)],
        GraphKind::Reaction => &[("strength", PortType::Unit)],
        GraphKind::Subgraph => &[],
    };
    if !required.is_empty() && !g.outputs.keys().map(String::as_str).eq(required.iter().map(|(n, _)| *n)) {
        ctx.err(Code::Schema, at, "/outputs");
    }
    let mut roots: BTreeSet<&str> = flow_nodes.clone();
    for (name, target) in &g.outputs {
        let path = ptr("/outputs", name);
        if !is_local_id(name) {
            ctx.err(Code::IdFormat, at, &path);
        }
        let Some((n, p)) = split_port(target) else {
            ctx.err(Code::Schema, at, &path);
            continue;
        };
        let Some(sig) = sigs.get(n) else {
            if !g.nodes.iter().any(|x| x.id.as_str() == n) {
                ctx.err(Code::RefMissing, at, &path);
            }
            continue;
        };
        roots.insert(n);
        match sig.outputs.iter().find(|(q, _)| q == p).map(|(_, t)| *t) {
            None => ctx.err_node(Code::PortType, at, &path, n),
            Some(Some(t)) => {
                if let Some((_, want)) = required.iter().find(|(r, _)| r == name)
                    && !t.connects_to(*want)
                {
                    ctx.err_node(Code::PortType, at, &path, n);
                }
            }
            Some(None) => {}
        }
    }

    // A cycle inside the graph: report the node where the depth-first walk closes it.
    let mut next: BTreeMap<&str, Vec<&str>> = BTreeMap::new();
    for (a, b) in &links {
        next.entry(a).or_default().push(b);
    }
    let index: BTreeMap<&str, usize> = g.nodes.iter().enumerate().map(|(i, n)| (n.id.as_str(), i)).collect();
    let mut state: BTreeMap<&str, u8> = BTreeMap::new(); // 1 on stack, 2 done
    for n in &g.nodes {
        let mut stack: Vec<(&str, usize)> = vec![(n.id.as_str(), 0)];
        if state.contains_key(n.id.as_str()) {
            continue;
        }
        state.insert(n.id.as_str(), 1);
        while let Some((cur, k)) = stack.pop() {
            let succ = next.get(cur).map(Vec::as_slice).unwrap_or(&[]);
            if k < succ.len() {
                stack.push((cur, k + 1));
                let s = succ[k];
                match state.get(s) {
                    Some(1) => {
                        let i = index[s];
                        ctx.err_node(Code::GraphCycle, at, &ptr("/nodes", i), s);
                    }
                    Some(_) => {}
                    None => {
                        state.insert(s, 1);
                        stack.push((s, 0));
                    }
                }
            } else {
                state.insert(cur, 2);
            }
        }
    }

    // Nodes that feed no output and no step of a reaction. Skipped when a node did not
    // parse: its links are unknown.
    if !all_parsed {
        return;
    }
    let mut prev: BTreeMap<&str, Vec<&str>> = BTreeMap::new();
    for (a, b) in &links {
        prev.entry(b).or_default().push(a);
    }
    let mut alive: BTreeSet<&str> = BTreeSet::new();
    let mut todo: Vec<&str> = roots.into_iter().collect();
    while let Some(n) = todo.pop() {
        if alive.insert(n) {
            todo.extend(prev.get(n).into_iter().flatten().copied());
        }
    }
    for (i, n) in g.nodes.iter().enumerate() {
        if !alive.contains(n.id.as_str()) {
            ctx.err_node(Code::DeadBranch, at, &ptr("/nodes", i), n.id.as_str());
        }
    }
}

fn node_ref(ctx: &mut Ctx, r: &Ref, kind: Kind, at: At, path: &str, node: &str) {
    match Kind::from_prefix(r.kind()) {
        Some(k) if k == kind => {
            if ctx.data.exists(k, r.as_str()) {
                ctx.used.insert(r.0.clone());
            } else {
                ctx.err_node(Code::RefMissing, at, path, node);
            }
        }
        _ => ctx.err_node(Code::RefKind, at, path, node),
    }
}

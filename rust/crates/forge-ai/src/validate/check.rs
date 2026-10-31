//! Checks across files: references, ranges the types cannot express, the manifest,
//! schedules, localisation and unused resources. Graphs are in `graph.rs`.

use std::collections::{BTreeMap, BTreeSet};

use super::load::{Data, Res, parse_value};
use super::{Code, Sink, ptr};
use crate::data::*;

/// Where a check is: the file and resource it reports against.
#[derive(Clone, Copy)]
pub(crate) struct At<'a> {
    pub file: &'a str,
    pub res: Option<&'a str>,
}

pub(crate) struct Ctx<'a> {
    pub data: &'a Data,
    pub sink: &'a mut Sink,
    /// Resources somebody refers to, for `W_UNUSED`.
    pub used: BTreeSet<String>,
    // Lists from project.json; `None` when it did not load, then nothing is checked against them.
    tags: Option<BTreeSet<&'a str>>,
    buckets: Option<BTreeSet<&'a str>>,
    day_parts: Option<BTreeSet<&'a str>>,
    seasons: Option<BTreeSet<&'a str>>,
    main_loc: Option<&'a BTreeMap<String, String>>,
}

impl<'a> Ctx<'a> {
    pub fn err(&mut self, code: Code, at: At, path: &str) {
        self.sink.push(code, at.file, at.res, path, None);
    }

    pub fn err_node(&mut self, code: Code, at: At, path: &str, node: &str) {
        self.sink.push(code, at.file, at.res, path, Some(node));
    }

    /// A reference that must point to an existing resource of one of `kinds`.
    pub fn reference(&mut self, r: &Ref, kinds: &[Kind], at: At, path: &str) {
        match Kind::from_prefix(r.kind()) {
            Some(k) if kinds.contains(&k) => {
                if self.data.exists(k, r.as_str()) {
                    self.used.insert(r.0.clone());
                } else {
                    self.err(Code::RefMissing, at, path);
                }
            }
            _ => self.err(Code::RefKind, at, path),
        }
    }

    pub fn tag(&mut self, t: &Tag, at: At, path: &str) {
        if self.tags.as_ref().is_some_and(|s| !s.contains(t.as_str())) {
            self.err(Code::RefMissing, at, path);
        }
    }

    pub fn bucket(&mut self, b: &LocalId, at: At, path: &str) {
        if self.buckets.as_ref().is_some_and(|s| !s.contains(b.as_str())) {
            self.err(Code::RefMissing, at, path);
        }
    }

    pub fn manifest(&self) -> Option<&'a Manifest> {
        self.data.manifest.as_ref().map(|m| &m.value)
    }

    /// Ids in a list must be unique; reports every repeat.
    pub fn unique<'s>(&mut self, ids: impl IntoIterator<Item = &'s str>, at: At, base: &str, field: &str) {
        let mut seen = BTreeSet::new();
        for (i, id) in ids.into_iter().enumerate() {
            if !seen.insert(id) {
                let path = ptr(&ptr(base, i), field);
                let path = path.trim_end_matches('/').to_owned();
                self.err(Code::IdDuplicate, at, &path);
            }
        }
    }

    pub fn cond(&mut self, c: &Cond, at: At, path: &str, depth: u32) {
        if depth > 8 {
            self.err(Code::Range, at, path);
            return;
        }
        match c {
            Cond::StockGte { tag, .. } => self.tag(tag, at, &ptr(&ptr(path, "stock_gte"), "tag")),
            Cond::HasTag { tag } => self.tag(tag, at, &ptr(&ptr(path, "has_tag"), "tag")),
            Cond::FlagIs { .. } | Cond::TimeBetween { .. } => {}
            Cond::All(list) | Cond::Any(list) => {
                let name = if matches!(c, Cond::All(_)) { "all" } else { "any" };
                let base = ptr(path, name);
                if list.is_empty() || list.len() > 16 {
                    self.err(Code::Range, at, &base);
                }
                for (i, c) in list.iter().enumerate() {
                    self.cond(c, at, &ptr(&base, i), depth + 1);
                }
            }
            Cond::Not(inner) => self.cond(inner, at, &ptr(path, "not"), depth + 1),
            Cond::NeedBelow { need, .. } => {
                self.reference(need, &[Kind::Need], at, &ptr(&ptr(path, "need_below"), "need"))
            }
            Cond::NeedAbove { need, .. } => {
                self.reference(need, &[Kind::Need], at, &ptr(&ptr(path, "need_above"), "need"))
            }
            Cond::HasTrait { trait_ } => {
                self.reference(trait_, &[Kind::Trait], at, &ptr(&ptr(path, "has_trait"), "trait"))
            }
            Cond::DayPartIs { part } => {
                if self.day_parts.as_ref().is_some_and(|s| !s.contains(part.as_str())) {
                    self.err(Code::RefMissing, at, &ptr(&ptr(path, "day_part_is"), "part"));
                }
            }
            Cond::EventPhaseIs { event, phase } => {
                let base = ptr(path, "event_phase_is");
                self.reference(event, &[Kind::Event], at, &ptr(&base, "event"));
                if let Some(ev) = self.data.events.get(event.as_str())
                    && !ev.value.phases.iter().any(|p| p.id == *phase)
                {
                    self.err(Code::RefMissing, at, &ptr(&base, "phase"));
                }
            }
        }
    }

    pub fn conds(&mut self, list: &[Cond], at: At, base: &str) {
        for (i, c) in list.iter().enumerate() {
            self.cond(c, at, &ptr(base, i), 1);
        }
    }

    pub fn effects(&mut self, list: &[Effect], at: At, base: &str) {
        for (i, e) in list.iter().enumerate() {
            let p = ptr(base, i);
            match e {
                Effect::StockAdd { tag, .. } => self.tag(tag, at, &ptr(&ptr(&p, "stock_add"), "tag")),
                Effect::SetFlag { .. } => {}
                Effect::EmitStimulus { stimulus } => {
                    self.reference(stimulus, &[Kind::Stimulus], at, &ptr(&ptr(&p, "emit_stimulus"), "stimulus"))
                }
                Effect::NeedAdd { need, .. } => {
                    self.reference(need, &[Kind::Need], at, &ptr(&ptr(&p, "need_add"), "need"))
                }
                Effect::EmotionAdd { emotion, .. } => {
                    self.reference(emotion, &[Kind::Emotion], at, &ptr(&ptr(&p, "emotion_add"), "emotion"))
                }
            }
        }
    }

    /// Arguments of a step or an input against their signature in the manifest.
    pub fn args(
        &mut self,
        args: &BTreeMap<String, serde_json::Value>,
        sig: &BTreeMap<String, ArgType>,
        at: At,
        base: &str,
        node: Option<&str>,
    ) {
        let declared: BTreeMap<&str, (ArgType, bool)> =
            sig.iter().map(|(k, t)| (k.trim_end_matches('?'), (*t, !k.ends_with('?')))).collect();
        for (name, value) in args {
            let path = ptr(base, name);
            let Some((ty, _)) = declared.get(name.as_str()) else {
                self.sink.push(Code::Manifest, at.file, at.res, &path, node);
                continue;
            };
            self.arg_value(*ty, value, at, &path, node);
        }
        for (name, (_, required)) in &declared {
            if *required && !args.contains_key(*name) {
                self.sink.push(Code::Manifest, at.file, at.res, base, node);
            }
        }
    }

    fn arg_value(&mut self, ty: ArgType, value: &serde_json::Value, at: At, path: &str, node: Option<&str>) {
        let v = value.clone();
        let (file, res) = (at.file, at.res);
        let wrong = |s: &mut Sink| s.push(Code::Manifest, file, res, path, node);
        match ty {
            ArgType::Unit | ArgType::Real | ArgType::Int | ArgType::Minutes if !value.is_number() => wrong(self.sink),
            ArgType::Unit => {
                parse_value::<Unit>(v, self.sink, file, res, path, node);
            }
            ArgType::Real => {
                parse_value::<Real>(v, self.sink, file, res, path, node);
            }
            ArgType::Int => {
                parse_value::<IntAny>(v, self.sink, file, res, path, node);
            }
            ArgType::Minutes => {
                parse_value::<Minutes>(v, self.sink, file, res, path, node);
            }
            ArgType::Bool if !value.is_boolean() => wrong(self.sink),
            ArgType::Bool => {}
            ArgType::Tag | ArgType::LogicalName | ArgType::Ref if !value.is_string() => wrong(self.sink),
            ArgType::Tag => {
                if self.tags.as_ref().is_some_and(|s| !s.contains(value.as_str().unwrap_or(""))) {
                    self.sink.push(Code::RefMissing, file, res, path, node);
                }
            }
            ArgType::LogicalName => {
                let name = value.as_str().unwrap_or("");
                let known = self.manifest().is_some_and(|m| {
                    m.animations.iter().chain(&m.sounds).chain(&m.effects).any(|n| n.as_str() == name)
                });
                if !known {
                    wrong(self.sink);
                }
            }
            ArgType::Ref => {
                let s = value.as_str().unwrap_or("");
                if !is_id(s) {
                    self.sink.push(Code::IdFormat, file, res, path, node);
                } else {
                    let r = Ref(s.to_owned());
                    let ok = Kind::from_prefix(r.kind()).is_some_and(|k| self.data.exists(k, s));
                    if ok {
                        self.used.insert(r.0);
                    } else {
                        self.sink.push(Code::RefMissing, file, res, path, node);
                    }
                }
            }
        }
    }

    fn header<T: Header>(&mut self, r: &Res<T>, id: &str) {
        let at = At { file: &r.file, res: Some(id) };
        for (i, t) in r.value.tags().iter().enumerate() {
            self.tag(t, at, &ptr("/tags", i));
        }
        let name = r.value.name().as_str();
        if let Some(loc) = self.main_loc
            && !loc.contains_key(name)
        {
            self.err(Code::LocMissing, at, "/name");
        }
    }
}

pub(crate) fn run(data: &Data, sink: &mut Sink) {
    let project = data.project.as_ref().map(|p| &p.value);
    let main_lang = project.and_then(|p| p.languages.first()).map(|l| l.as_str());
    let mut ctx = Ctx {
        data,
        sink,
        used: BTreeSet::new(),
        tags: project.map(|p| p.tags.iter().map(|t| t.as_str()).collect()),
        buckets: project.map(|p| p.buckets.iter().map(|b| b.id.as_str()).collect()),
        day_parts: project.map(|p| p.day_parts.iter().map(|d| d.id.as_str()).collect()),
        seasons: project.map(|p| p.calendar.seasons.iter().map(|s| s.as_str()).collect()),
        main_loc: main_lang.and_then(|l| data.loc.get(l)),
    };

    if let Some(p) = &data.project {
        project_file(&mut ctx, p);
    }
    if let Some(m) = &data.manifest {
        let at = At { file: &m.file, res: None };
        let m = &m.value;
        ctx.unique(m.inputs.iter().map(|i| i.id.as_str()), at, "/inputs", "id");
        ctx.unique(m.steps.iter().map(|s| s.id.as_str()), at, "/steps", "id");
        ctx.unique(m.channels.iter().map(|c| c.as_str()), at, "/channels", "");
    }

    macro_rules! each {
        ($map:ident, $f:ident) => {
            for (id, r) in &data.$map {
                ctx.header(r, id);
                $f(&mut ctx, At { file: &r.file, res: Some(id) }, &r.value);
            }
        };
    }
    each!(needs, need);
    each!(traits, trait_);
    each!(archetypes, archetype);
    each!(curves, curve);
    each!(objects, object);
    each!(actions, action);
    each!(jobs, job);
    each!(roles, role);
    each!(stimuli, stimulus);
    each!(emotions, emotion);
    each!(reactions, reaction);
    each!(events, event);
    each!(schedules, schedule);
    for (id, r) in &data.scenarios {
        ctx.header(r, id);
    }
    for (id, r) in data.graphs.iter().chain(&data.subgraphs) {
        ctx.header(r, id);
        super::graph::check(&mut ctx, id, r);
    }

    // Nobody refers to it: a likely leftover. Needs, archetypes, objects, events,
    // reactions, roles and scenarios are roots and are never reported. With a file
    // that did not parse its references are unknown, so nothing is reported.
    if data.incomplete {
        return;
    }
    let leaves = [
        (Kind::Curve, data.curves.iter().map(|(i, r)| (i, &r.file)).collect::<Vec<_>>()),
        (Kind::Action, data.actions.iter().map(|(i, r)| (i, &r.file)).collect()),
        (Kind::Trait, data.traits.iter().map(|(i, r)| (i, &r.file)).collect()),
        (Kind::Schedule, data.schedules.iter().map(|(i, r)| (i, &r.file)).collect()),
        (Kind::Graph, data.graphs.iter().map(|(i, r)| (i, &r.file)).collect()),
        (Kind::Subgraph, data.subgraphs.iter().map(|(i, r)| (i, &r.file)).collect()),
        (Kind::Stimulus, data.stimuli.iter().map(|(i, r)| (i, &r.file)).collect()),
        (Kind::Emotion, data.emotions.iter().map(|(i, r)| (i, &r.file)).collect()),
        (Kind::Job, data.jobs.iter().map(|(i, r)| (i, &r.file)).collect()),
    ];
    for (_, list) in leaves {
        for (id, file) in list {
            if !ctx.used.contains(id.as_str()) {
                ctx.sink.push(Code::Unused, file, Some(id), "", None);
            }
        }
    }
}

fn project_file(ctx: &mut Ctx, p: &Res<Project>) {
    let at = At { file: &p.file, res: None };
    let v = &p.value;
    if v.format.0 != 1 {
        // Format 0 opens only after `migrate::migrate_to`.
        ctx.err(Code::Schema, at, "/format");
    }
    if v.buckets.is_empty() || v.buckets.len() > 5 {
        ctx.err(Code::Range, at, "/buckets");
    }
    ctx.unique(v.buckets.iter().map(|b| b.id.as_str()), at, "/buckets", "id");
    if v.day_parts.is_empty() {
        ctx.err(Code::Range, at, "/day_parts");
    }
    ctx.unique(v.day_parts.iter().map(|d| d.id.as_str()), at, "/day_parts", "id");
    for (i, w) in v.day_parts.windows(2).enumerate() {
        if w[1].start <= w[0].start {
            ctx.err(Code::Range, at, &ptr(&ptr("/day_parts", i + 1), "start"));
        }
    }
    if v.calendar.seasons.is_empty() {
        ctx.err(Code::Range, at, "/calendar/seasons");
    }
    ctx.unique(v.calendar.seasons.iter().map(|s| s.as_str()), at, "/calendar/seasons", "");
    ctx.unique(v.tags.iter().map(|t| t.as_str()), at, "/tags", "");
    match v.languages.first() {
        None => ctx.err(Code::Range, at, "/languages"),
        Some(l) if !ctx.data.loc.contains_key(l.as_str()) => {
            ctx.sink.push(Code::LocMissing, &format!("loc/{l}.json"), None, "", None)
        }
        Some(_) => {}
    }
}

fn need(ctx: &mut Ctx, at: At, v: &Need) {
    ctx.reference(&v.urgency, &[Kind::Curve], at, "/urgency");
    for (i, m) in v.decay_modifiers.iter().enumerate() {
        ctx.cond(&m.when, at, &ptr(&ptr("/decay_modifiers", i), "when"), 1);
    }
}

fn trait_(ctx: &mut Ctx, at: At, v: &Trait) {
    for (i, m) in v.modifiers.iter().enumerate() {
        let kind = match m.field {
            ModField::Decay => Kind::Need,
            ModField::Weight => Kind::Action,
            ModField::Sensitivity => Kind::Stimulus,
        };
        ctx.reference(&m.target, &[kind], at, &ptr(&ptr("/modifiers", i), "target"));
    }
}

fn archetype(ctx: &mut Ctx, at: At, v: &Archetype) {
    let mode = v.mode.unwrap_or(Mode::Life);
    if mode == Mode::Life && v.schedule.is_none() {
        ctx.err(Code::Schema, at, "/schedule");
    }
    if mode == Mode::Life && v.think_ticks.is_some() {
        ctx.err(Code::Schema, at, "/think_ticks");
    }
    for (i, t) in v.traits.iter().enumerate() {
        ctx.reference(t, &[Kind::Trait], at, &ptr("/traits", i));
    }
    if let Some(s) = &v.schedule {
        ctx.reference(s, &[Kind::Schedule], at, "/schedule");
    }
    for (from, to) in &v.curves {
        ctx.reference(from, &[Kind::Curve], at, &ptr("/curves", from));
        ctx.reference(to, &[Kind::Curve], at, &ptr("/curves", from));
    }
}

fn curve(ctx: &mut Ctx, at: At, v: &Curve) {
    let p = &v.params;
    let formula = [("m", p.m.is_some()), ("k", p.k.is_some()), ("b", p.b.is_some()), ("c", p.c.is_some())];
    match v.kind {
        CurveKind::Linear | CurveKind::Poly | CurveKind::Logistic => {
            for (name, present) in formula {
                if !present {
                    ctx.err(Code::Schema, at, &ptr("/params", name));
                }
            }
            if p.points.is_some() {
                ctx.err(Code::Schema, at, "/params/points");
            }
        }
        CurveKind::Step | CurveKind::Points => {
            for (name, present) in formula {
                if present {
                    ctx.err(Code::Schema, at, &ptr("/params", name));
                }
            }
            match &p.points {
                None => ctx.err(Code::Schema, at, "/params/points"),
                Some(pts) if pts.len() < 2 || pts.len() > 32 => ctx.err(Code::Range, at, "/params/points"),
                Some(pts) => {
                    for (i, w) in pts.windows(2).enumerate() {
                        if w[1][0] <= w[0][0] {
                            ctx.err(Code::Range, at, &ptr("/params/points", i + 1));
                        }
                    }
                }
            }
        }
    }
    if v.input_range[0] >= v.input_range[1] {
        ctx.err(Code::Range, at, "/input_range");
    }
}

fn object(ctx: &mut Ctx, at: At, v: &Object) {
    if v.slots.is_empty() || v.slots.len() > 32 {
        ctx.err(Code::Range, at, "/slots");
    }
    ctx.unique(v.slots.iter().map(|s| s.id.as_str()), at, "/slots", "id");
    for (tag, s) in &v.stock {
        ctx.tag(tag, at, &ptr("/stock", tag));
        if s.start.0 < 0 || s.start.0 > s.max.0 {
            ctx.err(Code::Range, at, &ptr("/stock", tag));
        }
    }
    for (i, a) in v.advertises.iter().enumerate() {
        let base = ptr("/advertises", i);
        ctx.reference(&a.action, &[Kind::Action], at, &ptr(&base, "action"));
        for (j, s) in a.slots.iter().enumerate() {
            if !v.slots.iter().any(|x| x.id == *s) {
                ctx.err(Code::RefMissing, at, &ptr(&ptr(&base, "slots"), j));
            }
        }
    }
}

fn action(ctx: &mut Ctx, at: At, v: &Action) {
    ctx.bucket(&v.bucket.default, at, "/bucket/default");
    for (i, o) in v.bucket.overrides.iter().enumerate() {
        let base = ptr("/bucket/overrides", i);
        ctx.cond(&o.when, at, &ptr(&base, "when"), 1);
        ctx.bucket(&o.bucket, at, &ptr(&base, "bucket"));
    }
    ctx.conds(&v.conditions, at, "/conditions");
    if v.steps.is_empty() || v.steps.len() > 16 {
        ctx.err(Code::Range, at, "/steps");
    }
    ctx.unique(v.steps.iter().map(|s| s.id.as_str()), at, "/steps", "id");
    for (i, s) in v.steps.iter().enumerate() {
        let base = ptr("/steps", i);
        match ctx.manifest().and_then(|m| m.steps.iter().find(|x| x.id == s.type_)) {
            Some(sig) => ctx.args(&s.args, &sig.args, at, &ptr(&base, "args"), None),
            None if ctx.manifest().is_some() => ctx.err(Code::Manifest, at, &ptr(&base, "type")),
            None => {}
        }
    }
    ctx.effects(&v.effects, at, "/effects");
    ctx.effects(&v.on_cancel, at, "/on_cancel");
    ctx.reference(&v.graph, &[Kind::Graph], at, "/graph");
    if let Some(g) = ctx.data.graphs.get(v.graph.as_str())
        && g.value.kind != GraphKind::Utility
    {
        ctx.err(Code::RefKind, at, "/graph");
    }
    if let (Some(anim), Some(m)) = (&v.animation, ctx.manifest())
        && !m.animations.contains(anim)
    {
        ctx.err(Code::Manifest, at, "/animation");
    }
}

fn job(ctx: &mut Ctx, at: At, v: &Job) {
    ctx.reference(&v.action, &[Kind::Action], at, "/action");
    ctx.tag(&v.work, at, "/work");
    for (i, m) in v.materials.iter().enumerate() {
        ctx.tag(&m.tag, at, &ptr(&ptr("/materials", i), "tag"));
    }
}

fn role(ctx: &mut Ctx, at: At, v: &Role) {
    for job in v.priorities.keys() {
        ctx.reference(job, &[Kind::Job], at, &ptr("/priorities", job));
    }
    if let Some(s) = &v.schedule {
        ctx.reference(s, &[Kind::Schedule], at, "/schedule");
    }
}

fn stimulus(ctx: &mut Ctx, at: At, v: &Stimulus) {
    let Some(m) = ctx.manifest() else { return };
    for (i, c) in v.channels.iter().enumerate() {
        if !m.channels.contains(c) {
            ctx.err(Code::Manifest, at, &ptr("/channels", i));
        }
    }
}

fn emotion(ctx: &mut Ctx, at: At, v: &Emotion) {
    if let Some(c) = &v.curve {
        ctx.reference(c, &[Kind::Curve], at, "/curve");
    }
}

fn reaction(ctx: &mut Ctx, at: At, v: &Reaction) {
    ctx.reference(&v.stimulus, &[Kind::Stimulus], at, "/stimulus");
    if let Some(f) = &v.filter {
        ctx.cond(f, at, "/filter", 1);
    }
    ctx.reference(&v.graph, &[Kind::Graph], at, "/graph");
    if let Some(g) = ctx.data.graphs.get(v.graph.as_str())
        && g.value.kind != GraphKind::Reaction
    {
        ctx.err(Code::RefKind, at, "/graph");
    }
    match &v.response {
        Response::Effects(list) => ctx.effects(list, at, "/response"),
        Response::Action(a) => {
            ctx.reference(&a.action, &[Kind::Action], at, "/response/action");
            ctx.bucket(&a.bucket, at, "/response/bucket");
        }
    }
}

fn event(ctx: &mut Ctx, at: At, v: &Event) {
    let s = &v.start;
    let calendar = s.season.is_some() || s.day.is_some();
    let periodic = s.every_days.is_some() || s.offset.is_some();
    let forms = calendar as u8 + periodic as u8 + s.trigger.is_some() as u8;
    if forms != 1 || (calendar && (s.season.is_none() || s.day.is_none())) || (periodic && s.every_days.is_none()) {
        ctx.err(Code::Schema, at, "/start");
    }
    if let Some(season) = &s.season
        && ctx.seasons.as_ref().is_some_and(|s| !s.contains(season.as_str()))
    {
        ctx.err(Code::RefMissing, at, "/start/season");
    }
    if let (Some(day), Some(p)) = (s.day, ctx.data.project.as_ref())
        && day.0 > p.value.calendar.days_per_season.0
    {
        ctx.err(Code::Range, at, "/start/day");
    }
    if let Some(t) = &s.trigger {
        ctx.cond(t, at, "/start/trigger", 1);
    }
    if v.phases.is_empty() || v.phases.len() > 12 {
        ctx.err(Code::Range, at, "/phases");
    }
    ctx.unique(v.phases.iter().map(|p| p.id.as_str()), at, "/phases", "id");
    for (i, ph) in v.phases.iter().enumerate() {
        let base = ptr("/phases", i);
        for (j, a) in ph.actions.iter().enumerate() {
            let p = ptr(&ptr(&base, "actions"), j);
            ctx.reference(&a.action, &[Kind::Action], at, &ptr(&p, "action"));
            ctx.bucket(&a.bucket, at, &ptr(&p, "bucket"));
        }
        for (j, job) in ph.jobs.iter().enumerate() {
            ctx.reference(job, &[Kind::Job], at, &ptr(&ptr(&base, "jobs"), j));
        }
        if let Some(sc) = &ph.schedule {
            ctx.reference(sc, &[Kind::Schedule], at, &ptr(&base, "schedule"));
        }
    }
    for (i, inc) in v.incompatible.iter().enumerate() {
        ctx.reference(&inc.event, &[Kind::Event], at, &ptr(&ptr("/incompatible", i), "event"));
    }
    if let Some(adm) = &v.admission {
        if let Some(m) = ctx.manifest()
            && !m.inputs.iter().any(|i| i.id == adm.input)
        {
            ctx.err(Code::Manifest, at, "/admission/input");
        }
        ctx.reference(&adm.curve, &[Kind::Curve], at, "/admission/curve");
    }
    ctx.effects(&v.on_cancel, at, "/on_cancel");
}

fn schedule(ctx: &mut Ctx, at: At, v: &Schedule) {
    let day = TimeOfDay::DAY as usize;
    let mut cover = vec![0u8; day];
    for b in &v.blocks {
        let (from, to) = (b.from.0 as usize, b.to.0 as usize);
        if from == to {
            cover[from] = 2; // an empty block is a mistake too
            continue;
        }
        let mut m = from;
        while m != to {
            cover[m] = cover[m].saturating_add(1);
            m = (m + 1) % day;
        }
    }
    if cover.iter().any(|&c| c != 1) {
        ctx.err(Code::ScheduleGap, at, "/blocks");
    }
}

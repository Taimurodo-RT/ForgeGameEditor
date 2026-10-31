//! Source files of `game/villagers/` (docs/ai/data-model.md, «Схемы основных ресурсов»).
//!
//! These types are the single source of the JSON Schema in `schema/v1/`. Unknown
//! fields are errors everywhere except `meta` and the body of a scenario.

use std::collections::BTreeMap;

use schemars::JsonSchema;
use serde::Deserialize;

use super::types::*;

/// Adds the common header (`id`, `schema`, `name`, `tags`, `meta`) to a resource.
macro_rules! resource {
    ($(#[$m:meta])* pub struct $name:ident { $($(#[$fm:meta])* pub $f:ident : $t:ty,)* }) => {
        $(#[$m])*
        #[derive(Clone, Debug, Deserialize, JsonSchema)]
        #[serde(deny_unknown_fields)]
        pub struct $name {
            pub id: Id,
            /// `<kind>/<version>`, for example `action/1`.
            pub schema: String,
            pub name: LocKey,
            pub tags: Vec<Tag>,
            /// Free data for your own needs; the runtime never reads it.
            #[serde(default)]
            pub meta: Option<serde_json::Value>,
            $($(#[$fm])* pub $f: $t,)*
        }

        impl Header for $name {
            fn id(&self) -> &Id { &self.id }
            fn schema(&self) -> &str { &self.schema }
            fn name(&self) -> &LocKey { &self.name }
            fn tags(&self) -> &[Tag] { &self.tags }
        }
    };
}

/// Access to the common header of any resource.
pub trait Header {
    fn id(&self) -> &Id;
    fn schema(&self) -> &str;
    fn name(&self) -> &LocKey;
    fn tags(&self) -> &[Tag];
}

// ---------------------------------------------------------------- operations

/// A condition. JSON form: an object with one key, the operation name.
#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case", deny_unknown_fields)]
pub enum Cond {
    // module `rules`, levels 1–3
    StockGte {
        tag: Tag,
        value: Real,
    },
    HasTag {
        tag: Tag,
    },
    FlagIs {
        flag: LocalId,
        value: bool,
    },
    TimeBetween {
        from: TimeOfDay,
        to: TimeOfDay,
    },
    All(Vec<Cond>),
    Any(Vec<Cond>),
    Not(Box<Cond>),
    // module `village`, level 3 only
    NeedBelow {
        need: Ref,
        value: NeedValue,
    },
    NeedAbove {
        need: Ref,
        value: NeedValue,
    },
    HasTrait {
        #[serde(rename = "trait")]
        trait_: Ref,
    },
    DayPartIs {
        part: LocalId,
    },
    EventPhaseIs {
        event: Ref,
        phase: LocalId,
    },
}

/// An effect. JSON form: an object with one key, the operation name.
#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case", deny_unknown_fields)]
pub enum Effect {
    // module `rules`
    StockAdd { tag: Tag, value: Real },
    SetFlag { flag: LocalId, value: bool },
    EmitStimulus { stimulus: Ref },
    // module `village`
    NeedAdd { need: Ref, value: Real },
    EmotionAdd { emotion: Ref, value: Real },
}

/// Module of `forge-ai` that owns an operation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum OpModule {
    Rules,
    Village,
}

impl Cond {
    pub fn module(&self) -> OpModule {
        match self {
            Cond::StockGte { .. }
            | Cond::HasTag { .. }
            | Cond::FlagIs { .. }
            | Cond::TimeBetween { .. }
            | Cond::All(_)
            | Cond::Any(_)
            | Cond::Not(_) => OpModule::Rules,
            _ => OpModule::Village,
        }
    }
}

impl Effect {
    pub fn module(&self) -> OpModule {
        match self {
            Effect::StockAdd { .. } | Effect::SetFlag { .. } | Effect::EmitStimulus { .. } => OpModule::Rules,
            _ => OpModule::Village,
        }
    }
}

// ---------------------------------------------------------------- project.json

/// `project.json`. It has no resource header: it is the root of the folder.
#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Project {
    /// Project format version. Format 0 (28.10.2026) is migrated, see `migrate`.
    pub format: Int<0, 1>,
    pub tick_minutes: Int<1, 1440>,
    pub think_interval: Int<1, 1440>,
    pub day_parts: Vec<DayPart>,
    pub calendar: Calendar,
    /// From 1 to 5 priority groups, strongest first.
    pub buckets: Vec<Bucket>,
    pub tags: Vec<Tag>,
    pub languages: Vec<LocalId>,
    #[serde(default)]
    pub inertia: Option<Num<10_000, 20_000>>,
    #[serde(default)]
    pub stay_ratio: Option<Unit>,
    #[serde(default)]
    pub memory_limit: Option<Int<8, 1024>>,
    #[serde(default)]
    pub far_think_minutes: Option<Minutes>,
    /// Tiles per second of world time.
    #[serde(default)]
    pub move_speed: Option<RealPos>,
    #[serde(default)]
    pub meta: Option<serde_json::Value>,
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct DayPart {
    pub id: LocalId,
    pub start: TimeOfDay,
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Calendar {
    pub seasons: Vec<LocalId>,
    pub days_per_season: IntPos,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum Interrupt {
    Now,
    StepBoundary,
    Walking,
    Never,
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Bucket {
    pub id: LocalId,
    pub threshold: Unit,
    pub interrupt: Interrupt,
}

// ---------------------------------------------------------------- resources

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct DecayModifier {
    pub when: Cond,
    pub mul: Real,
}

resource! {
    /// A need and how fast it falls.
    pub struct Need {
        pub start: NeedValue,
        pub decay_per_hour: RealNonNeg,
        #[serde(default)]
        pub decay_modifiers: Vec<DecayModifier>,
        pub crisis_below: NeedValue,
        pub urgency: Ref,
        #[serde(default)]
        pub icon: Option<LocalId>,
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum CurveKind {
    Linear,
    Poly,
    Logistic,
    Step,
    Points,
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct CurveParams {
    #[serde(default)]
    pub m: Option<Real>,
    #[serde(default)]
    pub k: Option<Real>,
    #[serde(default)]
    pub b: Option<Real>,
    #[serde(default)]
    pub c: Option<Real>,
    /// `[x, y]` pairs, x ascending, from 2 to 32 points.
    #[serde(default)]
    pub points: Option<Vec<[Unit; 2]>>,
}

resource! {
    /// A response curve.
    pub struct Curve {
        pub kind: CurveKind,
        pub params: CurveParams,
        pub input_range: [Real; 2],
        #[serde(default)]
        pub clamp: Option<bool>,
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum Category {
    Eat,
    Sleep,
    Work,
    Leisure,
    Social,
    Idle,
    Survival,
    Event,
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct BucketRule {
    pub when: Cond,
    pub bucket: LocalId,
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ActionBucket {
    pub default: LocalId,
    #[serde(default)]
    pub overrides: Vec<BucketRule>,
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Step {
    pub id: LocalId,
    /// Step name from the manifest.
    #[serde(rename = "type")]
    pub type_: LocalId,
    /// Checked against the step signature in the manifest.
    pub args: BTreeMap<String, serde_json::Value>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum Interruptible {
    StepBoundary,
    Never,
}

resource! {
    /// An action: conditions, steps, effects, scoring graph.
    pub struct Action {
        pub category: Category,
        pub bucket: ActionBucket,
        #[serde(default)]
        pub conditions: Vec<Cond>,
        pub steps: Vec<Step>,
        #[serde(default)]
        pub effects: Vec<Effect>,
        #[serde(default)]
        pub on_cancel: Vec<Effect>,
        pub interruptible: Interruptible,
        pub graph: Ref,
        #[serde(default)]
        pub animation: Option<LogicalName>,
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum Facing {
    N,
    E,
    S,
    W,
    Any,
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Slot {
    pub id: LocalId,
    pub offset: [Real; 2],
    pub facing: Facing,
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Stock {
    pub start: IntAny,
    pub max: IntAny,
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Advert {
    pub action: Ref,
    pub slots: Vec<LocalId>,
}

resource! {
    /// A smart object: slots, stock, advertised actions.
    pub struct Object {
        pub slots: Vec<Slot>,
        #[serde(default)]
        pub stock: BTreeMap<Tag, Stock>,
        pub advertises: Vec<Advert>,
        #[serde(default)]
        pub perception_radius: Option<Tiles>,
    }
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Material {
    pub tag: Tag,
    pub count: IntPos,
}

resource! {
    /// A job template for the job board.
    pub struct Job {
        pub action: Ref,
        pub work: Tag,
        #[serde(default)]
        pub urgency: Option<Unit>,
        #[serde(default)]
        pub materials: Vec<Material>,
        #[serde(default)]
        pub min_skill: Option<Int<0, 20>>,
    }
}

resource! {
    /// A villager role on the priority board.
    pub struct Role {
        /// Job → priority: 0 never, 1 first, 4 last.
        pub priorities: BTreeMap<Ref, Int<0, 4>>,
        #[serde(default)]
        pub schedule: Option<Ref>,
    }
}

/// When an event starts: by the calendar (`season` + `day` or `every_days` + `offset`)
/// or by a world condition (`trigger`). Exactly one form.
#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct EventStart {
    #[serde(default)]
    pub season: Option<LocalId>,
    #[serde(default)]
    pub day: Option<IntPos>,
    #[serde(default)]
    pub every_days: Option<IntPos>,
    #[serde(default)]
    pub offset: Option<Minutes>,
    #[serde(default)]
    pub trigger: Option<Cond>,
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Until {
    pub until: TimeOfDay,
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(untagged)]
pub enum PhaseDuration {
    Minutes(Minutes),
    Until(Until),
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct PhaseAction {
    pub action: Ref,
    pub bucket: LocalId,
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Phase {
    pub id: LocalId,
    pub duration: PhaseDuration,
    #[serde(default)]
    pub weights: BTreeMap<Category, Real>,
    #[serde(default)]
    pub actions: Vec<PhaseAction>,
    #[serde(default)]
    pub jobs: Vec<Ref>,
    #[serde(default)]
    pub schedule: Option<Ref>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum OverlapRule {
    Pause,
    Cancel,
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Incompatible {
    pub event: Ref,
    pub rule: OverlapRule,
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Admission {
    /// Input from the manifest.
    pub input: LocalId,
    pub curve: Ref,
}

resource! {
    /// A long event with phases (festival, siege, epidemic).
    pub struct Event {
        pub priority: Int<0, 100>,
        pub start: EventStart,
        pub phases: Vec<Phase>,
        #[serde(default)]
        pub incompatible: Vec<Incompatible>,
        #[serde(default)]
        pub admission: Option<Admission>,
        #[serde(default)]
        pub on_cancel: Vec<Effect>,
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum BlockKind {
    Sleep,
    Work,
    Free,
    Any,
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Block {
    pub from: TimeOfDay,
    pub to: TimeOfDay,
    pub kind: BlockKind,
}

resource! {
    /// A daily schedule.
    pub struct Schedule {
        pub blocks: Vec<Block>,
        pub weights: BTreeMap<BlockKind, BTreeMap<Category, Real>>,
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum ModField {
    Decay,
    Weight,
    Sensitivity,
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Modifier {
    /// A need (`decay`), an action (`weight`) or a stimulus (`sensitivity`).
    pub target: Ref,
    pub field: ModField,
    pub mul: Real,
}

resource! {
    /// A character trait: only multipliers, no logic of its own.
    pub struct Trait {
        pub modifiers: Vec<Modifier>,
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum Mode {
    Life,
    Choice,
}

resource! {
    /// A villager template (level 3) or a choice-mode creature (level 2).
    pub struct Archetype {
        #[serde(default)]
        pub mode: Option<Mode>,
        /// Only for `choice`: think every N core ticks.
        #[serde(default)]
        pub think_ticks: Option<IntPos>,
        /// Tiles per second of world time; without it `project.json` decides.
        #[serde(default)]
        pub move_speed: Option<RealPos>,
        #[serde(default)]
        pub traits: Vec<Ref>,
        #[serde(default)]
        pub category_weights: BTreeMap<Category, Weight>,
        /// Required for `life`.
        #[serde(default)]
        pub schedule: Option<Ref>,
        #[serde(default)]
        pub curves: BTreeMap<Ref, Ref>,
    }
}

resource! {
    /// A kind of stimulus and its channels.
    pub struct Stimulus {
        pub channels: Vec<LocalId>,
        pub radius: Tiles,
        pub duration: Minutes,
        pub interrupts: bool,
        pub strength: Unit,
    }
}

resource! {
    /// An emotion and its decay.
    pub struct Emotion {
        pub decay_minutes: IntPos,
        #[serde(default)]
        pub curve: Option<Ref>,
    }
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(untagged)]
pub enum Response {
    Effects(Vec<Effect>),
    Action(PhaseAction),
}

resource! {
    /// A rule of reaction to a stimulus.
    pub struct Reaction {
        pub stimulus: Ref,
        #[serde(default)]
        pub filter: Option<Cond>,
        pub graph: Ref,
        pub response: Response,
        pub memory_minutes: Minutes,
        pub cooldown: Minutes,
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum GraphKind {
    Utility,
    Reaction,
    Subgraph,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum PortType {
    Unit,
    Real,
    Bool,
    Entity,
    Flow,
    Tag,
}

impl PortType {
    /// Whether a link `from → to` is allowed: same type, `unit → real` or `bool → unit`.
    pub fn connects_to(self, to: PortType) -> bool {
        self == to || matches!((self, to), (PortType::Unit, PortType::Real) | (PortType::Bool, PortType::Unit))
    }
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Node {
    pub id: LocalId,
    pub op: LocalId,
    /// Checked per operation, see `validate::graph`.
    #[serde(default)]
    pub params: serde_json::Map<String, serde_json::Value>,
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Edge {
    /// `node.port`
    pub from: String,
    /// `node.port`
    pub to: String,
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct GraphInput {
    pub id: LocalId,
    #[serde(rename = "type")]
    pub type_: PortType,
}

resource! {
    /// A scoring or reaction graph, or a reusable subgraph.
    pub struct Graph {
        pub kind: GraphKind,
        pub nodes: Vec<Node>,
        #[serde(default)]
        pub edges: Vec<Edge>,
        /// Output name → `node.port`.
        pub outputs: BTreeMap<String, String>,
        /// Only for `subgraph`: its external ports.
        #[serde(default)]
        pub inputs: Vec<GraphInput>,
    }
}

/// A test scenario. Its body arrives with stage 2; until then only the header is checked.
#[derive(Clone, Debug, Deserialize, JsonSchema)]
pub struct Scenario {
    pub id: Id,
    pub schema: String,
    pub name: LocKey,
    pub tags: Vec<Tag>,
    #[serde(default)]
    pub meta: Option<serde_json::Value>,
}

impl Header for Scenario {
    fn id(&self) -> &Id {
        &self.id
    }
    fn schema(&self) -> &str {
        &self.schema
    }
    fn name(&self) -> &LocKey {
        &self.name
    }
    fn tags(&self) -> &[Tag] {
        &self.tags
    }
}

// ---------------------------------------------------------------- manifest.json

#[derive(Clone, Copy, Debug, PartialEq, Eq, Deserialize, JsonSchema)]
#[serde(rename_all = "snake_case")]
pub enum ArgType {
    Unit,
    Real,
    Int,
    Bool,
    Minutes,
    Tag,
    LogicalName,
    Ref,
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ManifestInput {
    pub id: LocalId,
    #[serde(rename = "type")]
    pub type_: PortType,
    /// Argument name → type; a name ending in `?` is optional.
    #[serde(default)]
    pub args: BTreeMap<String, ArgType>,
}

#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ManifestStep {
    pub id: LocalId,
    pub version: String,
    /// Argument name → type; a name ending in `?` is optional.
    #[serde(default)]
    pub args: BTreeMap<String, ArgType>,
    #[serde(default)]
    pub result_reasons: Vec<LocalId>,
}

/// `manifest.json`: what the Forge game can do. Forge writes it, nobody edits it by hand.
#[derive(Clone, Debug, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Manifest {
    pub format: Int<1, 1>,
    #[serde(default)]
    pub inputs: Vec<ManifestInput>,
    #[serde(default)]
    pub steps: Vec<ManifestStep>,
    #[serde(default)]
    pub channels: Vec<LocalId>,
    #[serde(default)]
    pub animations: Vec<LogicalName>,
    #[serde(default)]
    pub sounds: Vec<LogicalName>,
    #[serde(default)]
    pub effects: Vec<LogicalName>,
}

//! Source data format of `game/villagers/` and its JSON Schema.

pub mod resources;
pub mod types;

pub use resources::*;
pub use types::*;

/// Resource kinds: id prefix, folder and current schema version.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Kind {
    Need,
    Trait,
    Archetype,
    Curve,
    Object,
    Action,
    Job,
    Role,
    Stimulus,
    Emotion,
    Reaction,
    Event,
    Schedule,
    Graph,
    Subgraph,
    Scenario,
    Dialogue,
}

impl Kind {
    pub const ALL: [Kind; 17] = [
        Kind::Need,
        Kind::Trait,
        Kind::Archetype,
        Kind::Curve,
        Kind::Object,
        Kind::Action,
        Kind::Job,
        Kind::Role,
        Kind::Stimulus,
        Kind::Emotion,
        Kind::Reaction,
        Kind::Event,
        Kind::Schedule,
        Kind::Graph,
        Kind::Subgraph,
        Kind::Scenario,
        Kind::Dialogue,
    ];

    /// Id prefix: `need` in `need.food`.
    pub fn prefix(self) -> &'static str {
        match self {
            Kind::Need => "need",
            Kind::Trait => "trait",
            Kind::Archetype => "archetype",
            Kind::Curve => "curve",
            Kind::Object => "object",
            Kind::Action => "action",
            Kind::Job => "job",
            Kind::Role => "role",
            Kind::Stimulus => "stimulus",
            Kind::Emotion => "emotion",
            Kind::Reaction => "reaction",
            Kind::Event => "event",
            Kind::Schedule => "schedule",
            Kind::Graph => "graph",
            Kind::Subgraph => "subgraph",
            Kind::Scenario => "scenario",
            Kind::Dialogue => "dialogue",
        }
    }

    pub fn folder(self) -> &'static str {
        match self {
            Kind::Need => "needs",
            Kind::Trait => "traits",
            Kind::Archetype => "archetypes",
            Kind::Curve => "curves",
            Kind::Object => "objects",
            Kind::Action => "actions",
            Kind::Job => "jobs",
            Kind::Role => "roles",
            Kind::Stimulus => "stimuli",
            Kind::Emotion => "emotions",
            Kind::Reaction => "reactions",
            Kind::Event => "events",
            Kind::Schedule => "schedules",
            Kind::Graph => "graphs",
            Kind::Subgraph => "subgraphs",
            Kind::Scenario => "scenarios",
            Kind::Dialogue => "dialogues",
        }
    }

    pub fn from_prefix(p: &str) -> Option<Kind> {
        Kind::ALL.into_iter().find(|k| k.prefix() == p)
    }

    /// The value the `schema` field must have: `action/1`.
    pub fn schema_tag(self) -> String {
        format!("{}/{}", self.prefix(), SCHEMA_VERSION)
    }
}

/// Version of every resource schema in this release.
pub const SCHEMA_VERSION: u32 = 1;

/// JSON Schemas (draft 2020-12) for `schema/v1/`: file name → pretty JSON with a final LF.
pub fn json_schemas() -> Vec<(String, String)> {
    use schemars::{JsonSchema, generate::SchemaSettings};

    fn one<T: JsonSchema>(name: &str) -> (String, String) {
        let generator = SchemaSettings::draft2020_12().into_generator();
        let schema = generator.into_root_schema_for::<T>();
        let mut text = serde_json::to_string_pretty(&schema).expect("schema serializes");
        text.push('\n');
        (format!("{name}.json"), text)
    }

    vec![
        one::<Project>("project"),
        one::<Manifest>("manifest"),
        one::<Need>("need"),
        one::<Trait>("trait"),
        one::<Archetype>("archetype"),
        one::<Curve>("curve"),
        one::<Object>("object"),
        one::<Action>("action"),
        one::<Job>("job"),
        one::<Role>("role"),
        one::<Stimulus>("stimulus"),
        one::<Emotion>("emotion"),
        one::<Reaction>("reaction"),
        one::<Event>("event"),
        one::<Schedule>("schedule"),
        one::<Graph>("graph"),
        one::<Scenario>("scenario"),
    ]
}

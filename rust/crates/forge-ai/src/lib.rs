//! `forge-ai`: game AI core of Forge (docs/ai/). Logic only: no window, render or sound.
//!
//! Stage 0 of docs/ai/spec.md: the source data format, its JSON Schema, the validator
//! and the first format migration. Modules:
//! - [`num`]: fixed-point numbers with the rounding rules of data-model.md;
//! - [`data`]: types of the files in `game/villagers/`, the source of `schema/v1/`;
//! - [`validate`]: checks a project folder and reports coded diagnostics;
//! - [`migrate`]: project format migrations;
//! - [`act`]: the step contract (execution ids), shared by every AI level.

pub mod act;
pub mod data;
pub mod migrate;
pub mod num;
pub mod validate;

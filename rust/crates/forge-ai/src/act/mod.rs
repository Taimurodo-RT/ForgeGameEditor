//! `act`: the step contract. Knows nothing about villagers: an execution belongs to
//! an entity and an opaque owner token given by the module that started it.

pub mod execution;

pub use execution::*;

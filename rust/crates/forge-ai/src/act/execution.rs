//! Step execution ids and accepting answers from the game.
//!
//! The owner of an execution is an entity plus an opaque token given by the caller
//! (`utility` puts an `ActionInstanceId` there, `fsm` a state instance number).
//!
//! Rules:
//! - an entity has at most one current execution;
//! - an answer is accepted only for the current execution of its entity, any other
//!   answer is stale and ignored;
//! - loading a save bumps the epoch and reissues current steps with new ids, so
//!   answers to executions of the old session never match a current one;
//! - execution ids are kept out of the snapshot and the decision journal (the journal
//!   records entity, token and step number), so a run with a save and load in the
//!   middle gives the same journal as a run without it.

use std::collections::BTreeMap;

/// World entity. To be replaced by the Forge core `EntityId` once it exists in shared code.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct EntityId(pub u64);

/// Opaque owner token. `act` only stores it and hands it back.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct OwnerToken(pub u64);

/// Step number inside whatever the owner started.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct StepIndex(pub u16);

/// Execution id. Fields are private: the game gets it in a request and returns it
/// unchanged in the answer. `to_raw` and `from_raw` carry it across the boundary.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct ExecutionId {
    epoch: u32,
    seq: u64,
}

impl ExecutionId {
    pub fn to_raw(self) -> (u32, u64) {
        (self.epoch, self.seq)
    }

    pub fn from_raw(epoch: u32, seq: u64) -> Self {
        Self { epoch, seq }
    }
}

/// Failure reason: index into the step's `result_reasons` in the manifest.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ReasonIndex(pub u16);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Outcome {
    Done,
    Failed(ReasonIndex),
    Cancelled,
}

/// What the owner learns from an accepted answer.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Finished {
    pub entity: EntityId,
    pub owner: OwnerToken,
    pub step: StepIndex,
    pub outcome: Outcome,
    /// Core tick in which the game finished the step (`simulation_tick`).
    pub tick: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Accept {
    Current(Finished),
    /// Answer to a cancelled, replaced or old-session execution. Changes nothing.
    Stale,
}

/// Step request the core hands to the game.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Issued {
    pub id: ExecutionId,
    pub entity: EntityId,
    pub owner: OwnerToken,
    pub step: StepIndex,
}

/// Snapshot row: the current step of an entity, without the execution id.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SavedExecution {
    pub entity: EntityId,
    pub owner: OwnerToken,
    pub step: StepIndex,
}

#[derive(Clone, Copy, Debug)]
struct Active {
    id: ExecutionId,
    owner: OwnerToken,
    step: StepIndex,
}

/// Current executions. Iterated by ascending `EntityId`, no `HashMap`.
#[derive(Debug, Default)]
pub struct Executions {
    epoch: u32,
    next_seq: u64,
    active: BTreeMap<EntityId, Active>,
}

impl Executions {
    pub fn new() -> Self {
        Self::default()
    }

    /// Starts a step. A current execution of the entity is replaced and its id is
    /// returned second, so the game can cancel that step.
    pub fn start(&mut self, entity: EntityId, owner: OwnerToken, step: StepIndex) -> (Issued, Option<ExecutionId>) {
        let id = self.next_id();
        let replaced = self.active.insert(entity, Active { id, owner, step }).map(|old| old.id);
        (Issued { id, entity, owner, step }, replaced)
    }

    /// Cancels the current step of an entity on the core's decision. Returns the id to
    /// send as a cancel request; the game's answer to it is `Stale` afterwards.
    pub fn cancel(&mut self, entity: EntityId) -> Option<ExecutionId> {
        self.active.remove(&entity).map(|a| a.id)
    }

    /// Accepts an answer from the game.
    pub fn accept(&mut self, id: ExecutionId, outcome: Outcome, tick: u64) -> Accept {
        let entity = match self.active.iter().find(|(_, a)| a.id == id) {
            Some((entity, _)) => *entity,
            None => return Accept::Stale,
        };
        let a = self.active.remove(&entity).expect("found above");
        Accept::Current(Finished { entity, owner: a.owner, step: a.step, outcome, tick })
    }

    pub fn current(&self, entity: EntityId) -> Option<ExecutionId> {
        self.active.get(&entity).map(|a| a.id)
    }

    /// Snapshot rows, by ascending `EntityId`.
    pub fn save(&self) -> Vec<SavedExecution> {
        self.active.iter().map(|(entity, a)| SavedExecution { entity: *entity, owner: a.owner, step: a.step }).collect()
    }

    /// Loads a snapshot into a running table. The epoch grows and every current step is
    /// reissued with a new id, by ascending `EntityId`.
    pub fn load(&mut self, saved: &[SavedExecution]) -> Vec<Issued> {
        self.epoch = self.epoch.checked_add(1).expect("execution epoch overflow");
        self.next_seq = 0;
        self.active.clear();
        let mut rows = saved.to_vec();
        rows.sort_by_key(|r| r.entity);
        rows.into_iter().map(|r| self.start(r.entity, r.owner, r.step).0).collect()
    }

    fn next_id(&mut self) -> ExecutionId {
        let id = ExecutionId { epoch: self.epoch, seq: self.next_seq };
        self.next_seq += 1;
        id
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const A: EntityId = EntityId(1);
    const B: EntityId = EntityId(2);
    const T: OwnerToken = OwnerToken(10);

    #[test]
    fn current_answer_is_accepted_once() {
        let mut ex = Executions::new();
        let (req, replaced) = ex.start(A, T, StepIndex(0));
        assert_eq!(replaced, None);
        let got = ex.accept(req.id, Outcome::Done, 5);
        assert_eq!(
            got,
            Accept::Current(Finished { entity: A, owner: T, step: StepIndex(0), outcome: Outcome::Done, tick: 5 })
        );
        assert_eq!(ex.accept(req.id, Outcome::Done, 6), Accept::Stale);
    }

    #[test]
    fn cancelled_and_replaced_answers_are_stale() {
        let mut ex = Executions::new();
        let (first, _) = ex.start(A, T, StepIndex(0));
        let (second, replaced) = ex.start(A, T, StepIndex(1));
        assert_eq!(replaced, Some(first.id));
        assert_eq!(ex.accept(first.id, Outcome::Done, 1), Accept::Stale);
        assert_eq!(ex.cancel(A), Some(second.id));
        assert_eq!(ex.accept(second.id, Outcome::Cancelled, 2), Accept::Stale);
    }

    #[test]
    fn load_reissues_with_new_ids_and_rejects_old_session() {
        let mut ex = Executions::new();
        let (a_old, _) = ex.start(A, T, StepIndex(2));
        let (b_old, _) = ex.start(B, OwnerToken(11), StepIndex(0));
        let saved = ex.save();
        let reissued = ex.load(&saved);
        assert_eq!(reissued.len(), 2);
        assert_eq!(reissued[0].entity, A);
        assert_eq!(reissued[0].step, StepIndex(2));
        assert_ne!(reissued[0].id, a_old.id);
        assert_eq!(ex.accept(a_old.id, Outcome::Done, 3), Accept::Stale);
        assert_eq!(ex.accept(b_old.id, Outcome::Done, 3), Accept::Stale);
        assert!(matches!(ex.accept(reissued[1].id, Outcome::Done, 3), Accept::Current(Finished { entity: B, .. })));
    }

    #[test]
    fn same_inputs_give_same_ids() {
        let run = || {
            let mut ex = Executions::new();
            let mut ids = Vec::new();
            for e in [B, A, B] {
                ids.push(ex.start(e, T, StepIndex(0)).0.id.to_raw());
            }
            let saved = ex.save();
            ids.extend(ex.load(&saved).into_iter().map(|r| r.id.to_raw()));
            ids
        };
        assert_eq!(run(), run());
    }
}

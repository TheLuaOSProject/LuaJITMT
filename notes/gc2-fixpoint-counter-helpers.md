## GC2 fixpoint and phase counter helpers

The GC2 mark/weak handoff counters are updated by phase transitions, weak
completion, and the mark fixpoint bridge. This slice routes `fixpoint_rounds`,
`fixpoint_hits`, `mark_complete_runs`, `mark_complete_hits`,
`mark_complete_peer_waits`, `mark_to_weak`, `weak_complete_runs`,
`weak_complete_progress`, and `weak_to_sweep` through helper accessors in
`lj_obj.h`.

Runtime initialization and increments now use the helper family, while the
focused phase/traverse C tests read the same counters through acquire helpers.
The M3 worker scheduler guard rejects raw production C access to these
fixpoint/phase telemetry fields.

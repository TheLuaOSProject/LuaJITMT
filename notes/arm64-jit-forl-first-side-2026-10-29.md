# Bounded ARM64 FORL first-side child support (2026-10-29)

This branch implements one bounded first-side child selected inside an
integer `BC_FORL` body. It is an experimental capability for the exact
source, IR, and map shape described here, not general FORL side-trace parity
or multiple-side support.

## Safety proof

The source root is admitted only after the existing `arm64_ir_forl_shape`
semantic and post-RA proofs establish its loop update and integer range guard.
That root-wide certificate is insufficient for a child update: the selected
parent exit may be inside the body before the parent's guard is reached. The
selected snapshot must also bind the child's inherited index, stop, and step
to the exact loop slots.

At child FORL index-update construction, the experimental ARM64 recorder
re-emits the existing `rec_for_check(..., init=1)` stop bound before the loop
comparison. The semantic and post-RA child validators independently require
the exact local `stop <= INT32_MAX-step` guard (or
`stop >= INT32_MIN-step` for a negative step), sourced from the readonly
inherited stop SLOAD, after the index ADD and before the loop comparison.

Both validators also bind that guard's cold snapshot to the old inherited
index and a source retry point after body effects: either the `JFORL` itself,
or an in-body `JMP` that directly targets that exact `JFORL`. Retrying at the
child-entry body would apply body effects twice; restoring the ADD result
could resume with a wrapped index. The validation requires the index slot to
map to the source index SLOAD, and checks footer and map bounds before reading
them. A test mutation that maps the index to the ADD result and another that
moves the retry PC are rejected independently by semantic and post-RA checks.

The captured ordinary source example records the local range guard as IR ref
8 with cold snapshot PC 21. That PC is the in-body JMP targeting JFORL at PC
27. The index slot is slot 5 and maps to inherited old-index SLOAD ref 1; the
increment ADD is ref 7 and is absent from that slot. These PCs and IR refs are
evidence from the captured example only; production validation derives the
tuple from the copied source and trace.

The parent snapshot copies the admission token, source interval, generation,
selected map values, and bindings under the existing SMR protocol. The first
side limit remains unchanged. Root entry separately proves the exact FORL/FORI
pair, patched JFORL identity, saved FORL instruction, matching A registers,
valid forward FORI target, and continuation inside the loop body before native
entry traverses the root-linked child.

## Validation status

The focused production contract passed at default repetitions for smoke,
ARM64 helper, and arm64e helper profiles, including `gc-claim`, `scoped`, and
`full-flush`. It checks child publication, safe native exits, retirement, and
the source/IR/map/physical mutations. Log:
`/Users/frityet/Scratch/ljmt-forl-first-side-20261029-preserved/guard-snapshot-contract.log`.

Strict AppleClang `-Wall -Wextra -Werror` object builds of `lj_trace.c` and
`lj_asm.c` passed for ARM64 and arm64e+BTI with assertions, GDBJIT, and test
helpers. The ordinary ARM64 topology probe from root commit `1487ee15b` also
passed. Its integer FORL workload published root trace 1's root-linked side
trace 2 and matched its interpreter oracle through the recorded exits. Logs
and objects are under
`/Users/frityet/Scratch/ljmt-forl-first-side-20261029-preserved/`.

Boundary conformance is checked against a separate explicitly JIT-disabled Lua
oracle. For `start=INT32_MAX-1, stop=INT32_MAX, step=1`, results match; runtime
counters identify exits from the root, not the child. The safe near-maximum
case `start=INT32_MAX-2, stop=INT32_MAX-1, step=1` matches and enters the
attached child. At exact `INT32_MIN`, results match and exits use the root; at
`INT32_MIN+1`, results match and exits use the child. The negative-step source
decoder sign-extends the `KSHORT` immediate, so `-1` is decoded correctly.

The current root FORL certificate requires its stop-plus-step bound guard
before the initial loop comparison. Therefore exact MAX/MIN boundary inputs
leave through that root guard before entering the child edge. The child-local
failing guard has not been dynamically reached; the snapshot retry safety is
established structurally and its mutations are rejected. Native child
witnesses cover ordinary and safe near-boundary values, not a dynamically
failing child guard. Do not describe this bounded slice as complete FORL or
multiple-side parity.

A previous experiment accepted the child ADD using only the copied parent
admission token and selected map. That path was closed; the current code
requires an explicit local bound and exact old-index retry snapshot.

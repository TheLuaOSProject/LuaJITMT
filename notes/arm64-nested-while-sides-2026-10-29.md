# Nested while-loop sides — 2026-10-29

The ordinary topology probe at integration `45fc76a31` left both integer and
fractional while-loop third paths interpreted: one published side and 3888
repeated exits from that side in phase three. The matched x86_64 probe published
a second root-linked side. Results still agreed with the interpreted oracle.

The ARM64 recorder reached assembly, but copying the selected parent restart
state returned retry. Both `lj_trace_arm64_side_parent_snapshot` and
`trace_arm64_side_context_from_parent` unconditionally requested inherited
FORL control-slot sources for a nested parent. That helper correctly refuses
BC_LOOP. Call it only for BC_FORL, matching the neighboring FORL range proof.
The BC_LOOP path retains zeroed FORL fields and still validates the full copied
snapshot, immediate-parent edge, ancestry generations, source geometry, IR,
allocated locations, publication and native-entry certificates. No emitter,
bytecode semantics, lock policy or x86_64 path changes.

`tests/t-arm64-jit-while-side-adaptation.c` constructs separate integer and
fractional BC_LOOP roots with three actual nested side levels. It verifies
all immediate-parent exit targets, the canonical root, both independent copied
admission passes and their ancestry-negative mutations, selected deepest-side
native exits with a live peer, full-GC reentry, and unchanged code bytes,
addresses and generations for all four bodies. The existing side-admission
contract now runs this fixture in its bound-profile and explicit matrix modes.

Focused four-profile evidence is in
`/Users/frityet/Scratch/ljmt-while-side-20261029/four-profile-fixed-oracle.log`.
ARM64 and arm64e assertion/release profiles all pass; the driver returns zero
and restores ordinary assertion ARM64 without test helpers. The first fixture
attempt accidentally invoked its cloned oracle without arguments; that test
setup error was corrected before this matrix (initial log retained).

The ordinary focused candidate probe now publishes two root-linked sides for
both while cases and uses the deepest child natively. In phase three it records
one transition exit from the direct side and 81 deepest-side exits, replacing
3888 direct-side exits. `candidate.log` contains this no-helper run;
`full-topology.log` is the subsequent complete ordinary probe with every oracle
comparison passing. This is a bounded topology correction, not full JIT parity.
The unrelated MAP_JIT allocation-range failure still blocks integration
promotion and complete combined side-gate evidence.

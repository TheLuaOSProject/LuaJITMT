# ARM64 scalar LOOP capability

The experimental ARM64 gate now accepts scalar self-linked `BC_LOOP` roots by
operation, type, provenance, snapshot and allocation properties. It replaces
the exact mixed-NUM and pure-NUM whole-trace arrays; it does not change Lua
arithmetic, the recorder, register allocation, or emitted arithmetic. The
separate integer `FORL`, fixed `FUNCF`, exact `CALLXS`, and first-side
certificates remain in place.

## Production boundary

`src/lj_asm_arm64_admit.h` contains separate recorder-state and immutable
post-register-allocation passes. Each has its own opcode/type/operand switch,
constant traversal and snapshot validation. Neither accepts the other's result.
Only bounded bytecode-range helpers are reused. `BC_LOOP` dispatches directly
to the new pair, so no old exact-NUM policy can hide a failure.

The scalar family permits checked INT add/subtract/multiply, NUM
add/subtract/multiply/divide, INT-to-NUM widening, checked NUM-to-INT conversion,
same-type ordered/equality comparisons and PHIs. Typed stack inputs must be
within both the root frame and the live extent of their current guard
snapshot. Constants are typed KINT/KNUM entries; KNUM payload words are not
addressable IR values. A root is single-frame, self-linked, and contains one
full XPOLL immediately after LOOP. Snapshot slots, flags, scalar sources,
bytecode PCs, frame geometry and guard coverage are checked independently.

The allocator pass rejects fixed GPRs, wrong register classes, invalid hint
encodings, reserved spill slots and unlocated live values. INT spill/frame
geometry retains the existing pressure proof. A missing NUM multiply location
is permitted only for the ARM64 fused add/subtract consumers; snapshots still
need a concrete resolved location. NUM spills remain closed.

Register-only PHIs must have distinct registers and agree with their left
values. Their right values must occupy the PHI register, or the register of
another PHI copying the same RHS. A swap can use an earlier scalar as RHS:
`asm_phi_shuffle` changes its original register, while loop-scoped RENAMEs
preserve its right-side location. The checker resolves those RENAMEs at the
loop snapshot, after validating the entire rename suffix and snapshot stream.
Raw RHS register equality would incorrectly reject stock swaps and rotations.

Heap references, table/upvalue reads and writes, allocation, helper/native
calls, arbitrary frames, general side traces and NUM spills remain closed.
This slice introduces no new shared-heap loads and no new GC object roots.
It continues the existing native entry, XPOLL, snapshot and retirement
protocols; it is not general ARM64 JIT completion.

## Focused evidence

`tools/ci/arm64_jit_scalar_capability_contract.sh` compiles and executes
`tests/t-arm64-jit-scalar-capability.c` with the companion Lua program. Select
an existing matching archive with `LJ_ARM64_SCALAR_ARCHIVE` and set
`LJ_ARM64_SCALAR_ARCH` to `arm64` or `arm64e` (the latter uses BTI).
The archive needs the bootstrap, experimental JIT, assertion, and
`LJ_TRACE_TEST_HELPERS` build flags.

Fourteen native families cover mixed intermediates, multiple accumulators,
multiple integer widenings, NUM comparisons, checked INT overflow with live
NUM state, flush/re-recording, all three fused multiply/add/subtract
orientations, NUM equality/inequality, two-value swaps and three-value
rotations. Every family proves real native publication and exit, post-entry
profile interruption, STOPREQ unwinding, resumed native entry and full GC.
Changed inputs prove fractional/type, overflow and NaN guard exits with the
numeric snapshots restored. FMA cases inspect actual emitted fused operations
and the two absent MUL locations they consume.

The swap has two NUM PHIs, one earlier RHS and four RENAMEs. The rotation has
three NUM PHIs, two earlier RHS values and five RENAMEs. The shared-RHS Lua
example is folded by the loop optimizer into two NUM PHIs with no duplicate
RHS; it is not claimed as execution of a duplicate-PHI copy. The independent
synthetic IR fixture separately accepts a valid duplicate-RHS allocation and
rejects missing/duplicate register ownership.

Independent semantic and post-RA mutation buffers cover missing polls,
heap/call opcodes, invalid flags and frames, forward and KNUM-payload
references, fixed registers, missing locations, NUM/reserved spills,
PHI/register disagreement, rename scopes and snapshots. An immutable IR copy
ends exactly at a protected page: an unlocated multiply followed by a malformed
future add must reject before dereferencing the add's out-of-range operand.
Removing that specific bound in a disposable negative control caused a native
SIGBUS; the correct checker rejects it safely. Register-only corruption is
accepted by the semantic pass and rejected by the allocator pass; recorder
frame corruption is independently rejected by the semantic pass.

Real table-read, table-write, allocation, math-call and 36-live-NUM pressure
programs remain interpreted. The original integer spill fixture still proves
23 fixed, 23 dynamic and 26 heavy values plus its lifecycle transitions.

The four existing numeric runtime fixtures retain their numerical, ABI/SLOAD,
layout, emitted-code, exit and lifecycle assertions, including all 37 dynamic
argument profiles. Former whole-trace-grammar negatives now require native
scalar admission where the loop can self-link. One-iteration reversed DIV/SUB
calls retain their result and no-trace checks. New positive cases explicitly
reset hotcounts between prototypes so preceding short-loop abort penalties do
not suppress recording.

## Validation scope

The focused contract and numeric fixtures were exercised on native ARM64 and
arm64e/BTI in disposable builds. The independent synthetic IR contract also
passes on native ARM64 and compiles for arm64e/BTI. Focused `FORL`, `FUNCF`,
integer and pointer `CALLXS`, and first-side tests check that their separate
certificates remain usable. This note does not claim a complete umbrella gate
for these source changes; that integration result belongs in the checkpoint
record. No Rosetta validation was started for this slice.

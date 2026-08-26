# ARM64 initial integer-loop IR admission checkpoint (2026-08-26)

## Outcome and boundary

This checkpoint adds a pure, fail-closed IR classifier for the first macOS
ARM64 trace shape: an optimized, self-linked integer root starting at
`BC_LOOP`. It deliberately admits only the semantic IR observed in one real
native ARM64 `while`-loop trace. Tables, objects, allocation, barriers, helper
calls, FFI, floating-point values, raw memory, nested frames, side traces,
stitches, and numeric `FORL` roots all remain outside the policy.

This commit does **not** open the recorder or native VM entry. The precise
`LJ_ARM64_JIT_RECORDER_ADMISSION_FAIL_CLOSED` and
`LJ_ARM64_JIT_NATIVE_ENTRY_FAIL_CLOSED` gates remain closed. The checked-in
positive fixture calls the classifier on synthetic unpublished IR; it neither
publishes nor executes generated code.

`lj_asm_trace()` invokes `lj_asm_arm64_ir_admit()` before growing assembler
scratch IR, allocating the compact trace body, or reserving machine code. A
rejection stores the exact rejected opcode in `J->errinfo` and raises the
existing deterministic `LJ_TRERR_NYIIR` trace error. Rejected input therefore
cannot reach mcode commit, trace-slot publication, prototype rooting, or a
bytecode patch.

## Exact root and bytecode policy

An admitted trace must be an unpublished root owned by the current recorder:

- `J->parent`, `J->exitno`, and `T->root` are zero;
- `T->traceno` is nonzero, `T->link == T->traceno`, and the link type is
  `LJ_TRLINK_LOOP`;
- the start prototype is the recorder's exact `J->pt`, the recorder has the
  root `baseslot == 1 + LJ_FR2`, and frame/return depths are zero;
- `startpc` is non-null, exactly equals `J->startpc`, is aligned inside the
  starting prototype's non-empty overflow-checked bytecode range, and an
  acquire load of the whole current word equals `T->startins`; and
- the start instruction is exactly `BC_LOOP`.

The `BC_LOOP` displacement must point forward inside the same prototype to a
negative `BC_JMP`. That jump must target the `BC_LOOP` or an earlier bytecode,
and the LOOP A operand cannot exceed the prototype frame extent. This rejects
stale and internally inconsistent start metadata before allocation.

`BC_FORL` is rejected even when its IR would otherwise be integer-only. ARM64
currently patches it to `BC_JFORL`, whose taken edge reaches branch-recovery
rather than the strict direct-`BC_JLOOP` native admission site. Publishing a
FORL trace at this stage would create an unreachable body, not a supported
execution path.

The optimized topology must contain exactly one `IR_LOOP`, followed
immediately by exactly one `IR_XPOLL`. `J->loopref` must name that LOOP and the
XPOLL literal must be `1`, so later generated code checks the global phase
gate plus TG-local poll and profile-request publications. The newest snapshot
active at XPOLL must be the LOOP snapshot.

## Exact semantic IR policy

Only these operations may appear before register allocation:

- `BASE`, with exact `IRT_PGC`, zero operands, and no flags;
- 32-bit `KINT` constants;
- the exact fixed, flag-free, zero-operand `KPRI` entries at `REF_TRUE`,
  `REF_FALSE`, and `REF_NIL`;
- `SLOAD`, with exact `IRT_INT | IRT_GUARD`, optionally `IRT_ISPHI`, and exact
  mode `IRSLOAD_TYPECHECK`;
- `ADDOV`, with exact guarded integer type and optional `IRT_ISPHI`;
- guarded integer `LT` and `GT`;
- the one `LOOP` and one `XPOLL #1`; and
- a terminal suffix of flag-free integer `PHI` instructions.

The admitted `IR_CALL*` helper-ID set is empty. CALL opcodes retain explicit
classifier cases only so diagnostics can report a rejected helper ID. Every
other opcode falls through to rejection. In particular, `KNUM`, all NUM IR,
conversions, unchecked arithmetic, `SUBOV`/`MULOV`, `LE`/`GE`, equality,
`USE`, table/object/raw-memory IR, allocation, barriers, and FFI calls remain
closed.

Every dynamic operand must be a preceding admitted integer value producer:
only a validated `SLOAD` or `ADDOV`. Constants must name an actual admitted
`KINT`; fixed primitive slots cannot masquerade as numeric constants. A guard,
`USE`, structural instruction, terminal PHI, self-reference, forward
reference, or constant payload slot cannot be used as an integer value.

PHIs follow the grammar consumed by the ARM64 assembler's backward scan:

- all PHIs form one contiguous terminal suffix;
- every PHI has a left operand before LOOP and a right operand after XPOLL but
  before the suffix;
- both operands are matching integer value producers marked `IRT_ISPHI`;
- every marked producer is named by at least one PHI;
- each PHI has a unique left producer (duplicate right producers are allowed
  by the assembler); and
- the suffix is capped at `LJ_MAX_PHI`.

The cap bounds the exact producer-mark audit. Early/interleaved PHIs, duplicate
left mappings, cycles, forward edges, stray producer marks, and oversized
suffixes reject deterministically.

## Stack and snapshot policy

An SLOAD slot must be at or above `1 + LJ_FR2`, below the largest snapshot
`nslots`, and within the starting prototype's root frame after accounting for
the recorder base offset. The exact type-check mode excludes parent/frame/key
loads, conversion, readonly/inherit variants, and the recorder-private
scalar-evolution exception used by narrowed FORL.

Every snapshot is validated before assembly:

- map regions start at zero, are contiguous, and both current and next
  offsets are at most `nsnapmap` before any map read;
- after `nent`, every region contains exactly `1 + LJ_FR2` packed frame-link
  words;
- the packed low byte exactly equals `J->baseslot - 2`, and the decoded PC is
  aligned inside the same starting prototype;
- snapshot refs are valid and nondecreasing, and each guard has an active
  snapshot at or before its ref;
- every `topslot` equals the root prototype's `framesize`, while `nslots`
  stays within that frame plus the recorder base;
- compressed slots are strictly increasing, unique, and below `nslots`;
- ordinary entries start at `1 + LJ_FR2`, have no metadata flags, and name an
  admitted integer value emitted before the snapshot ref; and
- the only frame entry is the canonical root sentinel
  `SNAP(1, SNAP_FRAME | SNAP_NORESTORE, REF_NIL)`.

Ordinary `SNAP_NORESTORE` is intentionally rejected. A future widening may
permit it only for a proved same-slot, non-converting SLOAD; it must never be
accepted for a computed value whose omission would leave stale interpreter
state.

These checks prevent integer-looking IR from carrying object, traversal,
nested-frame, sunk-materialization, malformed-PC, or out-of-bounds snapshot
state into the assembler.

## Recorder and native evidence

The checked-in fixture builds an unpublished integer LOOP topology using real
prototype bytecode, then tests the pure classifier and the protected
`lj_asm_trace()` rejection path. It does not assemble the positive candidate.

Separately, a disposable overlay was used to observe and execute the first
candidate on this native Apple ARM64 host. That audit produced a self-linked
`BC_LOOP` trace with 13 semantic IR instructions, two post-RA RENAMEs, nine
snapshots, and 168 bytes of mcode. Its semantic sequence was:

```text
SLOAD int, SLOAD int, ADDOV int, ADDOV int, SLOAD int, GT int,
LOOP, XPOLL #1, ADDOV int, ADDOV int, LT int, PHI int, PHI int
```

Five direct entries returned the exact loop result; helper counters reported
`publishes=5`, `cleanups=0`, and the normal loop condition restored snapshot
8. The trace used no allocator spill: all values remained in registers and
the allocator ended at `SPS_FIRST` with `spadjust == 0`.

However, the overlay had removed the stock `BC_JLOOP` entry's
`sub sp, sp, #16`. Correct arithmetic did not make that safe: the common exit
stub stores its link register at `[sp]`, which aliases the interpreter C
frame's saved predecessor link unless JLOOP first reserves those fixed spill
slots. The exit handler can still restore the current frame and return the
right result while leaving the predecessor corrupted. The checked-in native
fixture must therefore retain the 16-byte reserve and assert that `L->cframe`
is unchanged across every direct entry, in addition to checking the result and
exit snapshot.

That disposable run is feasibility and policy-shape evidence only, not a
successful safe enter/exit proof and not a claim that this commit has enabled
execution. It exposed the exact policy above and the next required publication
gate.

## Validation in this checkpoint

`tools/ci/arm64_jit_ir_admission_contract.sh` verifies:

- classifier placement before scratch growth, compact-trace allocation, and
  mcode reservation;
- exact BC_LOOP/current-word/prototype/JMP geometry and real BC_FORL rejection;
- the complete integer opcode inventory and empty CALL helper allowlist;
- canonical fixed primitives and rejection of KNUM/NUM families;
- value-producer provenance, PHI marking/side/unique-left grammar, and the
  `LJ_MAX_PHI` bound;
- snapshot offset, packed-PC/base, slot, flag, frame-link, and XPOLL-snapshot
  constraints;
- the existing ARM64 acquire lowering for phase gate, TG poll, and TG profile
  request;
- an arm64e `-Wall -Wextra -Werror` compile of `lj_asm.c`; and
- a native positive/negative fixture whose protected rejection observes
  `LJ_TRERR_NYIIR` plus the rejected opcode in `J->errinfo`.

The corruption matrix covers start pointer/word/range/geometry failures,
BC_FORL, side/stitch topology, forbidden constants and opcodes, unsupported
SLOAD modes/types/ranges, guard and structural refs used as values, malformed
and duplicate-left PHIs, oversized PHIs, snapshot offset/PC/base/slot/flag
corruption, ordinary `SNAP_NORESTORE`, a newer XPOLL snapshot, and unsigned or
otherwise unsupported comparisons.

The focused native contract and `git diff --check` passed on the final
worktree. The combined experimental gate is rerun after rebasing this detached
checkpoint onto the current branch.

## Remaining gates before checked-in execution

This classifier is pre-register-allocation. It does not prove the positive
candidate assembles without spills, because register allocation may append
RENAMEs and assign spill slots after this check. Before default native entry
opens, a second post-RA/pre-publication gate must require the audited final
shape, `T->spadjust == 0`, and no allocator spill (`evenspill == SPS_FIRST`,
`oddspill == 0`). The direct `BC_JLOOP` path must reserve the ARM64 backend's
16 fixed spill bytes before branching to mcode. A real checked-in positive
fixture must then assemble, publish, enter, condition-exit, and prove that the
interpreter C-frame link is unchanged.

Root recording must be split from still-closed side/stitch admission; direct
loop entry must be split from still-closed function/stitch entry. The root
helper must revalidate self-link topology, zero-spill metadata, bytecode and
body identity on both passes. Deterministic phase-gate, poll, profile, reqmask,
flush/retirement, stale-JLOOP, slot-reuse, and successful arm64e PAUTH/BTI
races remain required before broadening beyond this integer LOOP.

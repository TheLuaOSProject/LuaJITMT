# AArch64 JIT first-side certificate, tranche 1 (2026-08-27)

## Scope

Status update: the synthetic post-RA hypothesis in this tranche was replaced
after a native abort-before-publication observation. See
`notes/aarch64-jit-first-side-postra-observation-2026-08-27.md` for the exact
x28-parent to x27-child shuffle and the resulting immutable certificate.

This tranche adds both pure assembler-side policy for one observed first-level
integer side trace and a dormant, read-only recorder-ingress certificate for
its admitted LOOP parent. It does **not** open side recording, assemble a
production side trace, publish a child, patch a parent exit, or make the side
trace enterable. `LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED` remains `1`.

`TRACE_ARM64_INT_SIDE_ADMITTED` reserves bit `0x80`, but no assembler or trace
path sets it. The pure helpers take copied views rather than `jit_State` or a
published `GCtrace`, so their mutation tests cannot accidentally exercise
publication or retirement.

The recorder-ingress helper is likewise not called from `trace_hotside()`,
`lj_trace_ins()`, or `trace_start()`. Its caller must eventually provide either
a trace-body SMR lease or the exact recorder token; the helper itself acquires
neither and changes no token, trace state, count, topology, bytecode, exit slot,
or publication field.

## Dormant parent-ingress certificate

`lj_trace_arm64_first_side_loop_valid()` separates the immutable selected
snapshot continuation from the current incoming bytecode PC. It double-captures
the published parent view around generation validation and admits only:

- the exact runnable trace-vector slot and trace number requested by the caller;
- a root LOOP trace with self link, no child, no next side, no retirement or
  entry gate, and exactly one root-admission bit (`INT_LOOP`, never `FORL`,
  `FUNCF`, or the reserved `SIDE` bit);
- separately allocated exit slots whose selected raw entry exactly equals the
  owning-global encoding of the trace's shared fallback; embedded-mcode tables,
  retargeted exits, malformed body/fallback geometry, and arm64e signatures made
  with a different discriminator are rejected;
- the caller-selected exit in range and not `SNAPCOUNT_DONE`, with bounded and
  contiguous map/footer extents, strictly increasing ordinary slots, valid IR
  references, no snapshot-entry flags, zero packed base delta, and a footer
  exactly equal to the caller's continuation;
- an exact live `JLOOP(parent)` plus immutable original `BC_LOOP` sidecar,
  matching backward `BC_JMP`, and a stable continuation instruction inside that
  loop's bytecode interval.

The canonical prototype dump identifies offset 13 as live `CGET`, not `JMP`.
The exact `BCINS_AD(BC_JMP, 0, 0)` requirement therefore applies only to
initialized side-recorder scratch in RECORD; applying it to the parent snapshot
continuation at IDLE/START would reject the observed valid ingress.

This metadata helper is intentionally a reusable structural LOOP-parent gate;
it does not hard-code one exit number, prototype size, or continuation offset.
The dedicated fixture supplies the observed parent exit 2 and offset 13. The
assembler-side semantic certificate remains the exact canonical terminal gate
for the 19-bytecode, four-snapshot child grammar.

The IDLE context additionally proves the current Lua function/frame, TG actor
and ownership, empty request/hook/stream surfaces, and no recorder owner/token.
OWNER/START requires the exact parent/exit and canonical continuation but does
not inspect stale pre-`lj_trace_ins()` fields. OWNER/RECORD permits a later
incoming PC in the same prototype while requiring initialized zero-depth side
scratch: a distinct child trace number, root equal to parent, exact synthetic
`BCINS_AD(BC_JMP, 0, 0)`, canonical start continuation, and base slot
`1+LJ_FR2`. `RECORD_1ST` is rejected because it is an ITERN-root state outside
this bounded LOOP side grammar.

The same reserved `SIDE` bit was added to root-entry admission's mutually
exclusive mask, so a mixed `LOOP|SIDE` certificate cannot enter as a root.

## Exact semantic grammar

The admitted child and parent trace numbers are nonzero, distinct stored trace
numbers. The numeric values are not fixed: `root == link == parent`, while the
`IR_BASE` parent and exit operands must match the view. The only admitted exit
is parent exit 2, entered from canonical `BC_JMP`, and the child root-links to
its parent.

The canonical immutable layout is:

- `nins = REF_BASE+7`, `nk = REF_TRUE-1`, `nsnap = 4`,
  `nsnapmap = 13`, `baseslot = 2`, `topslot = 5`;
- a 19-slot prototype with continuation positions 13, 3, 17, and 7;
- one `KINT +1` plus the exact true, false, and nil primitive constants;
- `BASE(parent, 2)`;
- inherited parent integer `SLOAD` of slot 4, mode `0x21`;
- guarded integer `SLOAD` of slot 5, mode `0x04`;
- guarded integer `ADDOV(slot5, +1)`;
- guarded integer `SLOAD` of slot 2, mode `0x04`;
- guarded integer `GT(slot2, add-result)`;
- terminal guarded `XPOLL(1)`;
- snapshot refs at `BASE+2`, `BASE+4`, `BASE+5`, and `BASE+6`, with map
  offsets 0/3/7/10, entry counts 1/2/1/1, slot counts 5/6/5/5, top slot 5,
  zero frame-base deltas, and exactly the observed slot-to-reference maps.

Snapshot `count` is deliberately outside this certificate because hot-exit
attempts mutate it after recording. Snapshot `mcofs` is also excluded from the
semantic identity. Tests prove that changing either field does not change
admission. A future pre-publication gate must independently require the final
snapshot count to be `SNAPCOUNT_DONE` at the correct lifetime checkpoint.

Packed continuation PCs are accepted only when the prototype range is aligned
and non-overflowing, every position is in range, the frame-base byte is zero,
and the expected address fits before the eight-bit packing shift.

## Original synthetic post-RA hypothesis (superseded)

At this tranche boundary, no side trace had yet reached live register
allocation. The original helper therefore hypothesized one trailing `NOP`, no
renames or spills, zero child/parent stack adjustment, top slot 5, and the
inherited slot-4 value remaining in the parent's register.

The later native probe confirmed every structural item except that final
same-register assumption. The parent map carries slot 4 in x28 while the side
head shuffles it into x27. The current helper now freezes that observed layout,
including the exact post-RA register and spill bytes. Those fields force the
current `asm_head_side()` algorithm onto its x28-to-x27 shuffle path. The later
[head-shuffle certificate](aarch64-jit-first-side-head-shuffle-certificate-2026-08-27.md)
now also freezes the exact parent-map provenance and emitted move. Both helpers
remain dormant and cannot set the side admission bit; a production call site
still needs pre-head validation under a live parent certificate. Parent
lifetime, authenticated linking, publication order, retirement, and wider IR
surfaces remain separate gates.

## Validation

`tools/ci/arm64_jit_side_ir_admission_contract.sh` builds the pure test against
the experimental archive, recompiles the assembler for arm64e, asserts that no
production call site or marker publication was added, and applies extensive
field, constant, IR, snapshot, footer, register, spill, and suffix mutations.
The existing ARM64 IR-admission and fail-closed contracts remain required.
The fail-closed umbrella invokes the side contract directly.

`tools/ci/arm64_jit_side_ingress_metadata_contract.sh` runs the dedicated
metadata/generation/owner mutation fixture, checks both public and test-helper
symbols, audits the helper for mutation surfaces, proves there are no production
call sites while the side recorder macro is closed, and compiles the checkpoint
and PAC-specific wrong-discriminator fixture for arm64e. The root-entry contract
also runs that fixture against its arm64e archive so the authenticated positive
and wrong-discriminator negative execute, rather than merely compile. The
fail-closed umbrella invokes this ingress contract directly.

Local validation on Apple Silicon passed on 2026-08-27:

- native ARM64 `-Wall -Wextra -Werror` compilation of the assembler and test;
- `tools/ci/arm64_jit_side_ir_admission_contract.sh`;
- `tools/ci/arm64_jit_fail_closed_gate.sh`, including its nested exact ARM64 IR
  admission and ARM64/arm64e entry, exit, retirement, and reuse contracts.

# ARM64 root-entry helper checkpoint (2026-08-26)

This checkpoint adds only the C half of the macOS ARM64 root-trace entry
protocol. Recording and native entry remain fail-closed, `vm_arm64.dasc` is
unchanged, and no successful trace entry is constructed or executed.

## Frozen contract

- `LJTraceRootEntry` is ARM64-only and is exactly 16 bytes: the exact
  `GCtrace *` in AAPCS64 `x0`, followed by the entry target in `x1`. The future
  VM caller must treat both as caller-clobbered results and reject unless both
  are non-null.
- `lj_trace_enter_root(J, pc, traceno, L, base, sourceop)` accepts only strict
  roots reached from `BC_JLOOP` or `BC_JFUNCF`. It validates the current TG,
  actor/state ownership, `cur_L`, non-dead status, stack/base/top geometry,
  idle interpreter state, and absence of an existing native-entry lease.
- A closed GC2 JIT gate rejects before publication and release-stores
  `jit_sweep_displaced=1`. An open gate clears a pending MARK auto-yield,
  release-publishes `tg->jit_base`, executes the SC fence, then acquire-rechecks
  the gate before reading trace metadata.
- Metadata is acquired from the current immutable `TraceVec` slot and requires
  exact trace number, runnable/unretired state, `root==0`, exact `startpc`, a
  source-compatible original instruction, and an exact still-patched bytecode
  word. Raw mcode must be non-null, non-empty, aligned, and non-wrapping.
- On arm64e, the independently acquired `mcauth` pointer must be non-null and
  strip to the acquired raw mcode address. The signed pointer is returned; the
  future VM must branch with authentication using the returned exact trace as
  modifier. A raw `BR`/`BRAAZ`, or mixing an unauthenticated load with `BRAA`,
  is outside this contract.
- After target acquisition the helper reacquires the TraceVec/slot and repeats
  identity, runnable, root, bytecode, start metadata, mcode/size, and PAUTH
  equality checks. Success alone writes `tg->tmpbuf.L=L` and leaves `jit_base`
  published. Every post-publication rejection reaches one release-clear label.
- Rejection does not rewrite stale bytecode. The future VM caller must reload
  the instruction and route JLOOP/JFUNCF fallbacks to the existing static or
  stale-startins redispatch paths. The JFUNCF call-frame checks, missing-argument
  fill, and base/top adjustment must complete before calling this helper.

## Evidence in this tranche

`tests/t-arm64-jit-root-entry.c` directly covers invalid preconditions without
clobbering a foreign `jit_base`, open-gate missing-metadata cleanup for both
source families, closed-gate displacement without publication, and the two SC
handshake orderings with deterministic pauses: the closer wins before intent,
or it observes the published intent and cannot reclaim.

`tools/ci/arm64_jit_root_entry_contract.sh` performs a clean native experimental
build, runs that fixture, checks the helper's source ordering and forbidden-call
surface, disassembles an out-of-line ABI caller to prove x0/x1 consumption with
no hidden x8 result pointer, and confirms the recorder still publishes no
trace. A future backend tranche must add the actual VM call sequence, the
JFUNCF root-stack adjustment, authenticated branch, rejection redispatch, and
final linked-image disassembly proof before any success entry is enabled.

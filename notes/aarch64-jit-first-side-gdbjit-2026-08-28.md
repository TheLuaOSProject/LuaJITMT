# ARM64 exact-first-side GDBJIT checkpoint (2026-08-28)

## Scope

This checkpoint admits optional GDB JIT metadata for the four already-certified
first-level ARM64 integer sides. It does not admit another side grammar,
side-of-side recording, stitching, or PERFTOOLS. The broad ARM64 side gate
remains closed. Ordinary builds are unchanged; the new ownership field exists
only in ARM64 builds compiled with `LUAJIT_USE_GDBJIT`.

## Publication transaction

The split GDBJIT API prepared in the preceding checkpoint is now part of the
exact first-side transaction:

1. After the parent/child publication plan and compact-body geometry have been
   validated, `lj_gdbjit_preparetrace()` builds optional private metadata from
   `J->cur` and authenticates the exact certified parent generation. This is
   before compact-body initialization and the ASM-to-PUBLISH seal, so allocation,
   filename-bound rejection, or parent-SMR contention cannot enter the finite
   publication suffix. A `NULL` result is metadata omission, not trace failure.
2. The side publisher installs mcode, the GC root, child slot, topology,
   terminal snapshot count, and finally the authenticated runnable parent edge.
   It releases its temporary side-reader lease while the recorder token still
   retains and serializes the published child.
3. Only then does `lj_gdbjit_committrace()` make its one nonallocating,
   nonwaiting descriptor try-lock attempt. The debugger callback therefore sees
   a runnable trace body after the final semantic edge. Success transfers the
   allocation to the child. Contention leaves the fully runnable child without
   optional debugger metadata.

The preparation pointer is token-private durable state in `jit_State`, rather
than a stack local. This is necessary because every pre-PUBLISH refusal ends in
`lj_trace_err()`, which unwinds the publishing callback. A successful commit
clears the pointer. Rollback or a commit miss leaves it for
`trace_terminal_release()`, which detaches the pointer before exposing IDLE,
hands off the recorder token, repairs dispatch, and only then calls
`lj_gdbjit_aborttrace()`. The abort no longer reads the trace target, so an
unpublished body may already be retired and a published child may already be
available to another recorder. This also keeps the pluggable allocator's free
path outside the recorder-token critical section.

VM-close preflight and every fresh recorder start assert that no preparation
survived terminal release. TG-owner teardown uses the same terminal helper, so
ordinary completion, synchronous rollback, asynchronous abort, and detach all
converge on the same deferred cleanup.

## Runtime proof

`tests/t-arm64-jit-gdbjit-prepare.c` retains its bounded root-registration and
private-preparation cases and now records a real exact exit-2 child in fresh Lua
universes. It covers:

- successful child registration and native re-entry;
- nonthrowing preparation-allocation omission with an otherwise identical
  runnable child;
- a held descriptor lock at commit, proving one miss, no replay, a runnable
  unregistered child, and exactly one deferred abort after terminal handoff;
- a forced refusal immediately after successful preparation, proving recorder
  error unwinding leaves no child and frees the private entry exactly once;
- a forced external error at the same boundary, proving owner-detach cleanup
  clears the durable preparation before propagating the original Lua error;
- scoped-child flush, which unregisters only the child and retains the root;
- full flush, which unregisters both registered traces;
- descriptor contention during scoped retirement, which retains the retired
  child and its debugger entry through a mature ordinary reclaim attempt, then
  unregisters and frees it when that same completed epoch is retried after the
  lock is released; and
- the otherwise-similar unsupported `ADDOV +3` root, which never reaches GDB
  preparation and never publishes a child.

Test-only callback readiness now also requires that the descriptor's target is
the attached nonzero runnable trace generation, not merely that the
process-global descriptor fields are non-null. For an exact child, the callback
also takes its own bounded SMR reader and reconstructs the published generation:
the certified parent and exit, terminal snapshot, topology and raw authenticated
parent edge must all name that child while the recorder token still retains it.
Root callback checks remain generic.

The rollback seam covers both ordinary numeric recorder failure and a
nonnumeric external Lua error. The latter unwinds out of `trace_state()` and
therefore exercises `lj_trace_abort_owner()` instead of `trace_abort()`. Both
paths prove that the exact reserved child slot is cleared, the parent snapshot
is nonterminal, the pending preparation is freed after IDLE/token handoff, and
the same trace number can immediately publish a registered child. Release-build
fail-stops reject a stale pending owner both before preparation and at fresh
recorder admission.

`tests/t-arm64-jit-gdbjit-first-side-smoke.c` is linked against genuinely
helper-free GDBJIT archives for ordinary arm64 and arm64e+BTI. It uses the
canonical GDB interface types shared from `lj_gdbjit.h` and proves the real
descriptor chain, distinct root/child ELF entries, terminal snapshot, raw
authenticated exit-2 word, decoded child target, PAUTH discriminator and BTI
entry. Repeated native calls retain the same generation. Scoped flush unregisters
only the child and restores the exact saved fallback representation; full flush
removes the root and leaves the process-global descriptor list empty. Every
observation also requires IDLE, no recorder token/owner, no pending preparation
and zero SMR readers.

The source contract pins prepare-before-compact, compact-before-PUBLISH,
final-edge-before-SMR-release-before-commit, and terminal detach-before-IDLE-
before-token-release-before-deferred-abort. It also verifies that GDBJIT opens
only the granular first-side gate while PERFTOOLS and the broad side gate remain
closed.

## Validation

Validated locally on native Apple Silicon macOS:

- `arm64_jit_gdbjit_prepare_contract.sh` on arm64 and arm64e+BTI, including all
  root and real-child success/omission/rollback/external-error/retirement cases,
  plus helper-free production archives and descriptor/edge/flush smoke tests;
- the complete `arm64_jit_fail_closed_gate.sh` umbrella from a clean restart,
  including the newly wired GDBJIT stage and every later native entry, exit,
  retirement, flush/reuse, recorder-safepoint, and VM-safepoint stage;
- `arm64_jit_first_side_production_contract.sh` smoke plus GC-claim, scoped, and
  full-flush modes on arm64 and arm64e+BTI;
- the macOS x86_64 platform build and binary smoke; and
- an additional macOS x86_64 build with `LUAJIT_USE_GDBJIT` enabled.

The GDB contract is now part of the native ARM64 umbrella gate and restores the
ordinary thin ARM64 helper build. The pre-existing ARM64
`ccall_rawchild_wait` and no-assert `topofs` unused-function/variable warnings
remain the only observed compiler warnings.

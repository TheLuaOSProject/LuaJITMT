# Raw unpublished JIT scratch retirement

Date: 2026-07-19

## Problem and object kind

Assembler retry/abort can leave `J->curfinal`, a compact allocation made by
`lj_trace_alloc()`. It contains a copied IR span, but it has never acquired the
semantic ownership of a published `GCtrace`: there is no trace number or slot,
prototype/root-spine edge, snapshot/snapmap publication, exit table, debugger
entry, executable mcode or native-entry pin. The live recorder's `J->cur` is the
semantic root for KGC operands until abort cleanup ends.

The old unpublished path routed this allocation through ordinary retired-trace
preservation. That could loop in `trace_preservebody()` while the recorder token
was held and an exclusive SMR owner needed the same token. It also interpreted
uninitialized/non-owned fields as if this were a published trace.

## Retirement protocol

`TRACE_RETIRED_UNPUBLISHED` is an immutable atomic kind bit. The recorder owner
clears `J->curfinal` first, sets the kind, performs a one-shot raw pre-mark,
claims the retirement epoch, closes native-pin admission, publishes the exact
allocation on the token-owned retire list, and performs a one-shot raw post-mark.
Either mark may lose SMR admission; the local owner before list publication and
the list owner afterward are the lifetime authority, while the miss reopens the
active root certificate. Neither mark decodes the IR or any child field.

All three runtime retirement sites clear the token-private pointer before this
handoff: assembler IR-buffer replacement, synchronous `trace_abort()` and
`lj_trace_abort_owner()`. `lj_trace_alloc()` initializes `snap`, `snapmap`, GC
links, optional pointer-auth/GDB fields and every other scratch discriminator to
NULL/zero so validation never reads indeterminate storage.

## Retire-list consumer audit

Every production traversal recognizes the immutable kind:

- GC root marking preserves only the exact allocation.
- Mature reclaim bypasses inbound-link scanning, slot release, debugger removal
  and root-spine unlink, then validates and destroys the exact allocation.
- Mcode-area reference scans report no reference for scratch bodies.
- Stale-start-instruction recovery skips them.
- Terminal pin preflight validates their shape; close skips debugger teardown.
- Runtime/terminal destruction never frees an exit table for this kind.
- Requeue and payload-preservation decisions remain raw and nonsemantic.

The strict validator requires zero trace/link/root identities, NULL semantic
side pointers, zero mcode size and pin count, CLOSED pin admission, a nonzero
retirement epoch, no debugger entry, and exactly the unpublished kind bit.
Runtime corruption requeues fail-closed; terminal corruption aborts rather than
guessing ownership.

## Adjacent recorder terminal ordering

Successful assembly and terminal error/owner-abort cleanup now publish INTERP
and IDLE, release the recorder token, and only then run ordinary dispatch repair.
First-area `MCODEAL` and trace-number exhaustion defer full flush until after
that release. `trace_start()` returns ACTIVE, IDLE or FLUSH_ALL so invalid
bytecode, GC gating, `PROTO_NOJIT` and max-trace exits cannot refresh dispatch
while token-held. Down-recursion retries only when the restarted recorder
actually remains non-IDLE. An asynchronously aborted `MCODELM` restart falls
through ordinary slot cleanup instead of publishing IDLE with a leaked pending
trace. A stale hot dispatch also checks `JIT_F_ON` before attempting the token.

## Evidence and remaining debt

The focused retire fixture invokes the real unpublished retirement path while
holding both the recorder token and `LJ_GC2_SMR_META_EXCLUSIVE`. A deliberately
unmarked KGC operand proves there is no semantic traversal. The call returns
with zero readers, unchanged activation, an unmarked but tagged/listed exact
body, and then reclaims it only after full trace grace.

`LJ_TRERR_SMRRETRY` separately identifies failed recorder/assembler SMR
admission. Only that known closed-gate path suppresses TRACE-abort callbacks;
ordinary `LJ_TRERR_RETRY` remains observable. Generic abort callbacks still run
while the recorder token is held and retain a close-after-check dependency race.
Relocating them must preserve `jit.dump` and user abort-event semantics and is
explicit follow-up work.

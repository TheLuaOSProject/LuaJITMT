# XPOLL continuation snapshot ordering

## Symptom

Under concurrent trace churn, `tests/t-jit-flush-thread-stress.lua` could return
an exact partial numeric-loop sum.  The failing native exit was a terminal
`IR_XPOLL` on a live side trace.  Its exit restored the bytecode position after
the loop while the loop accumulator still held an intermediate value.

## Cause

The recorder emitted terminal and deep-inlined-FUNCF `IR_XPOLL` guards before
adding their continuation snapshots.  Guard assembly selects the newest
snapshot at or before the guard IR reference, so those XPOLL guards inherited
an earlier snapshot.  For the observed side trace, that earlier snapshot was
the numeric-loop condition exit; the correct terminal continuation snapshot
existed one IR reference after the guard and could not be selected.

This was a recorder snapshot-ordering bug.  It did not require a stale trace
slot, stale JLOOP opcode, or retired machine-code entry.

## Fix and invariant

`lj_record_stop()` now publishes the canonicalized terminal continuation
snapshot before emitting its XPOLL.  `rec_func_xpoll()` does the same for a
deep inlined call continuation.  The optimized self-loop case is unchanged:
its XPOLL immediately follows `IR_LOOP` and intentionally uses the loop
snapshot.

The `t-jit-token` regression constructs both a terminal side trace and a
deep-inlined-FUNCF trace.  For every non-loop XPOLL it finds, it requires an
exact snapshot whose `snap.ref` equals the XPOLL IR reference.  This directly
checks the ordering contract independently of whether a concurrent close-gate
request happens to hit the guard during a test run.

## Validation

- `tools/ci/lua_test.sh m6_jit_token`: pass, including both new invariants.
- `tools/ci/lua_test.sh m5_jit_trace_publish`: pass.
- ASAN/assert stress after the change: 26 passes with no wrong arithmetic,
  three timeouts, and one independent unpublished-trace ownership assertion.
  The timeouts and ownership assertion remain separate lifecycle defects and
  are not counted as successful runs or as fixed by this change.

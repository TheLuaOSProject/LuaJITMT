# GC2/JIT full-collection sweep progress (2026-07-12)

## Failure

`tests/t-jit-token.c` could leave a secondary thread spinning forever inside
`collectgarbage("collect")` during SWEEP while the main thread waited on a
channel. The visible work queues were drainable and the sweep bridge was ready,
but two independent retry sources prevented SWEEP from reaching IDLE.

First, every 1 ms slice of a timed channel wait called
`lj_state_stack_pubrange()` again. In SWEEP, root publication conservatively
queues traversable objects even when their allocation mark already exists. The
collector's closing handshake woke the native waiter, the waiter republished the
same unchanged stack, and the next close attempt found a fresh active SSB. This
formed an unbounded handshake/stack-publication loop.

Second, a later cycle could retire an unreachable coroutine and repeatedly fail
terminal THREAD preparation because `state_gcprep_roots_absent()` read
`g->cur_L` directly. On x86_64 the VM publishes the authoritative current state
in `TGState.cur_L`; the process-global field is only a compatibility/bootstrap
mirror and may still name the last resumed coroutine. Treating that stale mirror
as a root kept one RETIRED coroutine and its arena quarantined forever.

## Fix

- Timed channel send/receive and rendezvous waits publish the stable Lua stack
  once per logical wait operation, immediately before the first native park.
  Repeated 1 ms parks do not mutate that stack. A later GC epoch still obtains
  complete coverage by scanning the native TG during its handshake.
- Terminal THREAD preflight uses `lj_tg_cur_L(g)` and then scans every registered
  TG's authoritative `cur_L`/`thread_L` roots. It no longer grants root authority
  to the stale process-global mirror.
- `t-thread-gcprep.c` now constructs a deliberately stale global mirror while
  the main TG authoritatively names the real main state and verifies that the
  unreachable coroutine reaches the terminal preparation queue.

## Validation

- focused `t-jit-token`: 20 consecutive passes;
- complete `m6_jit_token`, including secondary recording/entry/exit and ordinary
  x86_64 trace smoke: pass;
- `m4_thread_gcprep`, including the new stale-mirror case: pass.

The original five-second channel timeout was restored after diagnosis; the test
does not mask future progress failures with an extended timeout.

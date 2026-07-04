JIT pre-activation loop XPOLL elision, 2026-07-04:

- x64/GC64 loop traces now emit `IR_XPOLL` only when the runtime has a remote
  safepoint participant or an activation handoff in progress:
  `gc2.n_threads > 1`, `gc2.n_workers != 0`, `mt_entering != 0`, or
  `mt_active != 0`.
- Pre-activation traces can omit the loop poll because both activation paths
  flush existing traces before remote TGs can run: `threading.spawn()` flushes
  before latching `mt_active`, and `threading.gcworkers(n > 0)` flushes before
  publishing `gc2.n_workers` and before creating worker pthreads.
- Deep inlined Lua `FUNCF` XPOLL emission is unchanged. Active/entering MT and
  GC-worker traces still poll at loop backedges.
- `t-jit-token.c` checks the IR shape directly: a pre-activation loop trace has
  no `IR_XPOLL`, while a loop recorded with GC workers active still has one.
  `m6_jit_barrier_xpoll` and `m6_jit_xbar_xpoll` activate parked GC workers
  before recording so their behavior still exercises poll-region semantics.

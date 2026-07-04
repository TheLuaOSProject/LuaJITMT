GC-worker trace activation flush, 2026-07-04:

- `threading.gcworkers(n > 0)` now flushes existing JIT traces before publishing
  `gc2.n_workers` and before the parked worker TGs are created.
- The flush holds the JIT token until `gc2.n_workers` is nonzero. This closes
  the window where a pre-worker trace could survive into a runtime with native
  GC worker TGs that can participate in safepoint handshakes.
- The existing loop `IR_XPOLL` lowering is unchanged. This is a correctness
  activation boundary for future narrowing; removing or weakening loop polling
  still requires separate recorder/assembler changes and tests.
- Coverage:
  `m6_jit_gcworkers_activation_flush` checks the public Lua control surface,
  and `t-gc2-worker-scheduler.c` asserts from the wrapped `pthread_create`
  boundary that traces are already gone before a worker pthread is created.

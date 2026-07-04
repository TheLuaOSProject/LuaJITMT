# tab_insert_newkey Investigation

Focused `BENCH_SCALE=0.2` best-of-five runs still show `tab_insert_newkey`
around 145-160 ns/op in the fork versus roughly 75-80 ns/op for stock LuaJIT.
At `BENCH_SCALE=1`, the fork remains roughly 1.6-1.9x stock depending on run.

Three runtime changes were tested and rejected in this pass:

- A string-key-specialized `lj_tab_newkey()` path preserved the existing
  KEYLOCK/freecount/resize/barrier protocol but replaced generic key equality
  with `GCstr *` pointer equality. It did not materially improve the benchmark
  and added duplicated insertion logic, so it was removed.
- A local new-key barrier fast-skip avoided calling the GC2 weak-key and
  key-publication helpers in the common idle non-generational state. It was
  modestly faster in microbenchmarks but made `m5_tab_resize_stress` time out
  even with a 60s timeout, so it was removed. The existing helper calls remain
  the safe boundary for weak phase, active marking, idle generational remembered
  sets, and legacy black-table barriers.
- Carrying an `anchor key was nil` fact from the initial miss scan into the
  private new-key path avoided one release-build key reload and improved a
  focused stock comparison run (`tab_insert_newkey` around `1.82x` stock versus
  the usual `~2.0x` noisy guard result). It was removed because
  `m5_tab_resize_stress` timed out at both 30s and 60s. The old private path's
  key/value reload is a required revalidation point: activation state can
  change around the private predicate, and a racing publisher must not be
  overwritten based on a stale pre-predicate nil-key observation.

Useful current evidence:

- `string_intern` is not the current bottleneck in the stock guard; focused
  `LJ_BENCH_STOCK_FILTERS='string_intern tab_insert_newkey tab_hash_write'`
  showed `string_intern` much faster than stock while `tab_insert_newkey`
  remained high.
- With GC stopped, a direct one-shot new-key insertion probe improves only
  modestly, so the gap is not primarily GC stepping.
- `IR_NEWREF` still calls `lj_tab_newkey()` for insertion. Pre-MT primitive
  value stores after nonnumeric `NEWREF` already use the returned slot directly,
  so the remaining target is insertion/allocation/growth work, not the value
  store helper.

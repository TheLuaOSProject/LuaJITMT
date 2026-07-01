# table structural owner bridge follow-up

- A direct table-local owner conversion was prototyped but not kept: individual
  resize stress cases passed, while cumulative `m5_tab_resize_stress` exposed a
  timing-sensitive crash outside gdb/Valgrind. That indicates the current
  global token is still hiding at least one shared resize/GC/JIT bridge that
  must be separated before unrelated table resizes can safely run in parallel.
- No wait-path code change is kept here: both L-aware structural waits and a
  shorter retry interval exposed the same cumulative resize stress instability.
  The fixed 1 ms sleep remains a pending bridge gap.
- A future table-local owner patch should carry a dedicated unrelated-table
  resize stress, but that stress should land with the actual correctness fix so
  the default resize suite remains stable.

Verification:

- `make clean && make -j$(nproc)`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m5_tab_resize_stress`

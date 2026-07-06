M5 traced resize + table-library accumulation crash, 2026-07-06:

- Strengthening `t-tab-resize-stress.lua` so the JIT resize workers prove that
  they produced traces exposed a process-lifetime interaction between the
  traced resize cases and later table-library/metamethod cases.
- Focused `jitstore,jitread,jititer` passes. The non-JIT table/GC/traversal and
  table-library tail also passes by itself. The combined shape
  `jitstore,jitread,jititer,tableclear,tablelib,tablelibshift,metadispatch`
  can segfault nondeterministically.
- A symbolized gdb run caught the fault in a worker thread at
  `lj_vm_call_dispatch_f()` while `package_require_cp()` was requiring
  `table.clear`; sibling worker threads were waiting on the start channel.
- The suite now runs traced resize stress as `m5_tab_resize_jit_stress` and the
  default table resize stress as a non-JIT case set. This keeps CI/harness
  checks stable while preserving a direct local reproducer for the runtime bug:

```sh
LJ_M5_TAB_RESIZE_STRESS_CASES=jitstore,jitread,jititer,tableclear,tablelib,tablelibshift,metadispatch \
  tools/ci/lua_test.sh m5_tab_resize_stress
```

The next implementation pass should decide whether the underlying issue is a
trace-retirement/lifetime problem after secondary TG traces, or a package
preload/require publication problem made visible by prior trace churn.

Related weak-finalizer observation from the same test-tightening pass:
changing the weak-key finalizer reader to a traceable `while` loop and requiring
trace production exposed intermittent timeout/segfault behavior even at reduced
`256/800` reps. That path remains a weak-table/cdata lifetime bug candidate; the
stable suite keeps `weakfinjit` as a GC2 weak/finalizer resize stress until the
traced weak-table read contract is fixed.

# GC2 Active Barrier Marked-Child Filter Audit

2026-07-04 audit result: do not add a global active-barrier fast return for
already marked children without a narrower proof.

Experiment:

- Added a filter to `lj_gc2_barrier_obj_pair()`:
  `if (gc2_barrier_active_g(g) && lj_gc2_ismarked(g, child) > 0) return;`
- Also tried making `lj_gc2_ismarkedmem()` use relaxed atomic bitmap loads for
  the arena block/mark words, so the filter did not depend on an extra plain
  concurrent bitmap read.

Observed benefit:

- The focused `closures_upval` stock guard improved from roughly `2.36x` to
  `2.11x` on this machine.
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m6_jit_fnew_bump` passed.
- `tools/ci/lua_test.sh m3_gc_root_pending m6_jit_gcstep_pacing` passed.
- `tools/ci/lua_test.sh m8_weak` passed, including the paranoia sub-builds.

Failure:

- `tools/ci/lua_test.sh m8_weak m7_ffi_finreg m3_gc2_worker_scheduler` then
  spun in the JIT-enabled `t-ffi-gc-finreg.lua 6 240` case.
- After several minutes the child process was still consuming CPU with no wait
  syscalls. A non-interactive `gdb` snapshot showed execution under `lua_gc()`
  from JIT mcode.
- Killing the run and reverting only the filter restored the tree to the pushed
  FNEW fallback state.

Conclusion:

The repeated locked mark update is a real warm-path cost, but a global
"child mark bit is already set" test is not a sufficient substitute for the
current barrier semantics. The mark bit alone does not encode enough state about
the edge, phase, weak/finalizer interactions, or pending traversal ownership for
this broad call site. Future work should either target a narrower constructor
case with a documented root-publication proof and finalizer stress coverage, or
add explicit GC2 state that proves the child traversal obligation is already
owned.

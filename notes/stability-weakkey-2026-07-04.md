# Weak-key resize stability slice - 2026-07-04

Focused reproducer:

```
LJ_M5_TAB_RESIZE_STRESS_CASES=weakkey src/luajit -joff tests/t-tab-resize-stress.lua
```

What failed:

- GC2 could keep a busy owned `lua_State` on the grey deque after also setting
  `NEEDSCAN`. The pending scan counter already represents that work, so the
  duplicate grey entry could spin until the owner acknowledged.
- Dead-owner thread states had no owner capable of acknowledging `NEEDSCAN`.
  GC2 now treats those as quiescent stacks after validating the thread body and
  stack allocation.
- Stale arena bodies from cdata/proto/thread/function/table paths could still
  reach sweep/traversal through duplicate root/grey entries. Type-specific
  validators now reject bodies whose size metadata, stack range, ctype, or proto
  layout cannot be trusted.
- The GC2 grey ring can only represent `capacity` distinct outstanding slots.
  If cursor drift exposes a wider span, the owner repairs the span to the newest
  full ring before pop/grow/empty decisions.
- `collectgarbage("collect")` was allowed to advance arena sweep one incremental
  batch at a time. Forced full GC now drains pending arena sweep work at the
  sweep boundary while ordinary incremental steps keep their budget.
- Arena shutdown accounting is normalized at `close_state()` after all roots and
  runtime side structures are gone. Per-subtract underflow remains asserted; the
  close-time byte counter is not used as a live-object leak oracle after GC2 has
  already settled arena bodies.

Validation:

- `make -C src clean && make -C src -j$(nproc) CCDEBUG='-g -DLUA_USE_ASSERT'`
- 120 focused weak-key stress iterations before removing local diagnostics.
- 80 focused weak-key stress iterations after cleanup.
- Mixed resize stress:
  `weak,gcmark,gckey,weakkey,weakmeta,finalizer,metatable`
- Channel sanity check for `threading.channel`.
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m5_tab_keylock_lookup m5_tab_finreg_newkey_stale m5_tab_struct_owner m5_x64_tnew_empty_inline m6_jit_fnew_bump`
- `git diff --check`

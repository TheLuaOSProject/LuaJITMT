# x64 FNEW Active-Black Inline Path

Date: 2026-07-05

The traced x64 one-numeric-upvalue `FNEW` allocator mirrors the C bump helper's
standalone active-black arena-owned case. It stays inline when:

- `TG.mark_active != 0`
- `TG.alloc.alloc_black != 0`
- `global_State.gc2.legacy_mark_bridge == 0`

The inline path still falls back to `lj_func_newL_gc1num_forjit()` for active
white allocation and for coupled legacy mark cycles. The bridge case must keep
the C helper publication path because stock-suite GC rechain/TSETM tests can
force a full collection before the next closure-heavy metatable test; fresh
closures created while the bridge is active need the root-spine publication
seen by the classic mark side.

For the active-black case, both fresh traversable arena cells have their mark
bits set before Lua-visible publication. The inline path initializes the closure
and closed upvalue, clears both `nextgc` links, and skips the pending-root push
just like `func_bump_publish_pair()`. Inactive allocation continues to publish
the initialized pair through `TG.gcroot_pending`.

Focused coverage:

- `m6_jit_fnew_bump` now checks active-black traced inline allocation, active
  white fallback, legacy-bridge fallback, and direct C-helper pending-root
  elision.
- `m6_jit_cell_ops` keeps the stock Lua closure/upvalue identity checks on the
  traced FNEW path.
- `m8_weak m7_ffi_finreg m3_gc2_worker_scheduler m3_gc_root_pending` passed,
  including the JIT-enabled finalizer stress that rejected the earlier broad
  marked-child barrier shortcut.

Fresh-build stock guard after this change reported default `closures_upval`
geomean `2.042175`. The main benefit here is removing a needless helper
boundary under a proven active-black state; it is not a broad `closures_upval`
throughput fix.

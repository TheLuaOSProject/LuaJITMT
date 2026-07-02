# GC root pending publication batch

2026-07-01:
- New GC object publication now has a TG-local pending path:
  `lj_gc_linkobj_new()` pushes newly initialized objects onto
  `TGState.gcroot_pending` instead of CAS-prepending directly to
  `g->gc.root`.
- Root-list consumers call `lj_gc_flush_root_pending()`, which xchg-drains each
  TG pending stack and CAS-prepends the whole drained chain to the legacy root
  list. This keeps legacy sweep/finalizer visibility while removing the global
  root-list cache-line from normal new-object allocation.
- Existing-object relinks stay immediate through `lj_gc_linkobj()` or
  `lj_gc_linkobj_after()`: closed-upvalue relinks and finalizer requeues do not
  use the pending stack because those paths may already be using `nextgc` for
  another chain or need immediate root placement.
- Converted new-object sites include `lj_mem_newgco()`, C/Lua closures, tables,
  cdata, saved traces, and child lua_State objects. Open upvalues remain on the
  open-upvalue list until closed, as before.
- Flush points cover GC cycle start, sweep setup, root-chain sweep/unlink,
  GC2 arena sweep preservation/verification, FINREG cdata root unlink,
  root-oracle checks, TG detach/reclaim, and close-state/freeall scans.
- `m3_gc_root_pending` verifies explicit flush and full-GC flush of a
  stack-rooted pending table. It also verifies that child lua_State and regular
  userdata objects flush through the after-main queue, preserving the legacy
  `mainthread->nextgc` topology.
- The main-thread pre-MT fast path now links fresh objects onto
  `TGState.gcroot_pending` with a release store instead of a CAS loop while no
  secondary Lua thread has existed and no GC2 worker is running. Once
  `mt_active` is latched or GC workers are enabled, allocation uses the CAS
  pending stack again. This removes a single-thread allocation RMW without
  changing the active-MT publication protocol.
- Fresh child lua_State objects and regular userdata now use
  `TGState.gcroot_pending_after_main` and `lj_gc_linkobj_new_after_main()`.
  This removes the direct `mainthread->nextgc` CAS from coroutine/thread-state
  and userdata allocation without moving those objects ahead of `mainthread`.
- 2026-07-02 follow-up: `m3_gc_root_pending` now also pins the TLS/self fallback
  in `lj_gc_flush_root_pending()` with a synthetic current TG that is not linked
  into `gc2.tg_list`. That protects attach/detach-adjacent flush behavior where
  the current TG may be the only discoverable owner of pending roots.

This is a contention bridge, not the final ADR-4/plan bitmap-only object list:
legacy sweep still walks `g->gc.root` after publication, and every new object
after MT activation still takes a per-TG atomic stack push until inline bump
allocation and arena-owned object discovery replace this bridge.

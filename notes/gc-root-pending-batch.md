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
- Existing-object relinks may use the pending stack only after the caller has
  released ownership of any previous `nextgc` chain. Closed upvalues qualify
  after `lj_func_closeuv()` removes them from both open-upvalue lists.
  Finalizer requeues stay immediate because they are already moving through
  finalization-specific chains and need precise replacement in the legacy
  finalization topology.
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
  secondary Lua thread has existed, no secondary attach path is entering, and
  no GC2 worker is running. Once `mt_active`, `mt_entering`, or GC workers are
  visible, allocation uses the CAS pending stack again. This removes a
  single-thread allocation RMW without changing the active-MT publication
  protocol.
- Fresh child lua_State objects and regular userdata now use
  `TGState.gcroot_pending_after_main` and `lj_gc_linkobj_new_after_main()`.
  This removes the direct `mainthread->nextgc` CAS from coroutine/thread-state
  and userdata allocation without moving those objects ahead of `mainthread`.
- 2026-07-02 follow-up: `m3_gc_root_pending` now also pins the TLS/self fallback
  in `lj_gc_flush_root_pending()` with a synthetic current TG that is not linked
  into `gc2.tg_list`. That protects attach/detach-adjacent flush behavior where
  the current TG may be the only discoverable owner of pending roots.
- 2026-07-02 attach follow-up: `lj_tg_attach()` now drains pending-root stacks
  immediately after publishing the TG to `gc2.tg_list`. This closes the
  pre-attach publication gap for any future path that queues objects before its
  TG is globally discoverable. The root-pending fixture now synthesizes that
  state with the TG no longer TLS-current.
- 2026-07-02 entering-window follow-up: x64 inline empty-table `TNEW` uses the
  same single-producer predicate. During `mt_entering` it routes through
  `lj_tab_new0()`, whose pending-root publication then takes the atomic path.
  `m5_x64_tnew_empty_inline` verifies this with a runtime helper-call counter,
  not by inspecting DynASM source.
- 2026-07-03 follow-up: `lj_gc_linkobj_new_chain()` publishes a caller-owned
  run of freshly initialized objects with one pending-root operation. The first
  use is the traced one-numeric-upvalue `FNEW` helper, where the `GCfunc` and
  its fresh closed `GCupval` are born together and neither object is reachable
  until both payloads and edges have been release-published. The helper uses the
  known tail directly, so fallback/global-root publication does not walk
  uninitialized tail links.
- 2026-07-03 generic one-upvalue follow-up: the generic `FNEW` fallback now uses
  the same chain publication when the callee prototype has exactly one
  source/v4 local cell upvalue and the parent frame slot is still raw. This
  covers nonnumeric captures and numeric fallback cases without changing
  inherited-cell behavior: if the slot already contains `LJ_TUPVAL`, the normal
  shared-cell path still reuses that cell. The chain is release-published only
  after the fresh `GCfunc`, fresh `GCupval`, upvalue payload, function uv slot,
  and parent-slot promotion are initialized.
- 2026-07-03 multi-upvalue follow-up: when the generic `FNEW` path has already
  rooted the fresh function, newly snapshotted local upvalue cells are linked
  after that function instead of being pushed individually to the TG pending
  root head. This keeps allocation-failure safety because the function is
  already visible before later cell allocations can fail, while avoiding
  extra pending-head contention for multi-upvalue closures. Reused inherited
  cells and open legacy upvalues stay on their existing chains.
- 2026-07-03 closed-upvalue follow-up: `lj_gc_closeuv()` now queues the closed
  `GCupval` through the same pending object-list bridge after unlinking it from
  the thread-open chain and `g->uvhead`. The object list is the sweep/free
  spine, not the semantic liveness root set; reachable closures still mark the
  upvalue directly. The pending fixture now closes a legacy open upvalue and
  verifies the closed cell is pending until the next explicit flush.
- 2026-07-04 hint follow-up: `global_State.gcroot_pending_hint` is a
  conservative non-empty hint for `lj_gc_flush_root_pending()`. Pending-root
  C publishers set it before and after publishing a non-null pending stack head;
  x64 interpreter empty `TNEW` and traced one-numeric-upvalue `FNEW` do the
  same around their direct inline stores. The flusher clears the hint before
  scanning so concurrent producers can republish it, and it still checks the
  main TG and TLS-current TG pending fields directly before taking an empty
  return. This avoids repeated empty TG-list scans without making the hint an
  ownership or reachability authority.
- 2026-07-04 stability follow-up: specialized C bump helpers now publish through
  the shared `lj_gc_linkobj_new()` / `lj_gc_linkobj_new_chain()` path instead of
  open-coding a local release-store pending push. The helper still keeps the
  single-producer fast path when the common linker can prove it, but the proof is
  evaluated at publication time after object initialization and before any
  accounting assist. Pending-chain flush also severs malformed cycles while
  preserving the unique objects ahead of the cycle, so a corrupted pending stack
  cannot hang GC root publication.

This is a contention bridge, not the final ADR-4/plan bitmap-only object list:
legacy sweep still walks `g->gc.root` after publication, and every new object
after MT activation still takes a per-TG atomic stack push until inline bump
allocation and arena-owned object discovery replace this bridge.

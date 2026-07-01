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
  `lj_gc_linkobj_after()`: closed-upvalue relinks, userdata/thread anchoring,
  and finalizer requeues do not use the pending stack because those paths may
  already be using `nextgc` for another chain or need immediate root placement.
- Converted new-object sites include `lj_mem_newgco()`, C/Lua closures, tables,
  cdata, and saved traces. Open upvalues remain on the open-upvalue list until
  closed, as before.
- Flush points cover GC cycle start, sweep setup, root-chain sweep/unlink,
  GC2 arena sweep preservation/verification, FINREG cdata root unlink,
  root-oracle checks, TG detach/reclaim, and close-state/freeall scans.
- `m3_gc_root_pending` verifies explicit flush and full-GC flush of a
  stack-rooted pending table.

This is a contention bridge, not the final ADR-4/plan bitmap-only object list:
legacy sweep still walks `g->gc.root` after publication, and every new object
still takes a per-TG atomic stack push until inline bump allocation and
arena-owned object discovery replace this bridge.

## 2026-07-01 performance regression audit

Read-only subagent audits confirmed the major reports:

- Normal GC object allocation still CAS-prepends most objects to the global
  `g->gc.root` list. Legacy sweep remains authoritative for normal-object
  lifetime, so deleting root membership is not safe yet. A local rootless
  closure experiment crashed during module loading by losing reachable
  function/proto/template-table state; it was discarded.
- No-upvalue closure allocation was directly starting legacy GC work through
  `func_newL_interp_softgc()`. That amplified root-list sweep cost in closure
  churn. The helper was removed; `lj_gc_check_fixtop()` still handles regular
  legacy threshold and GC2 hard-limit assists.
- Recursive `fib30` behavior was a JIT trace-retention bug. The call-unroll
  abort path used scoped trace retirement and cleared the return trace slot
  before `trace_abort()` could self-link it as the stock blacklist entry.
  The recorder path now uses `lj_trace_flush_unlink()`, which unlinks/unpatches
  without retiring the slot. `m6_jit_recursive_call_unroll` covers this, and
  `t-vm-safepoint` now asserts this recorder path does not perform a scoped
  handshake or scoped slot retirement; public `jit.flush(1)` still does.
- x64 interpreter table stores remain helper-heavy once MT or GC2 marking is
  active, but the warm single-thread path now has bounded direct stores for
  existing stable array slots and existing string-key hash slots. Previous-nil
  in-bounds array slots without a metatable also route through that direct-store
  gate. The helper path is still required for active MT, weak tables,
  metatables, retiring generations, forwarded slots, marking barriers and slot
  growth.
- x64 `BC_TNEW` has no inline bump allocation. The current safe prerequisite is
  still exact object layout/bitmap/accounting/root-publication support; first
  slice should be empty-table only and fall back to the current helper when any
  condition is not statically safe.
- The benchmark regression guard was accounting-only. `m9_bench_stock_compare`
  now compares selected filters against an installed stock LuaJIT when
  `LJ_BENCH_STOCK_BIN` is set, and Linux CI runs it with a wide threshold to
  catch catastrophic gaps without making normal platform CI dependent on tight
  timing.

Follow-up order:

1. Done as an interim bridge: new-object publication uses a TG-local
   pending-publication stack drained before legacy root-list consumers. This
   keeps stock sweep/finalizer semantics while removing the global root-list
   cache line from normal allocations; bitmap-only sweep and zero-atomic bump
   allocation remain separate follow-up work.
2. Continue reducing x64 interpreter store helper use only where the existing
   direct-store gate proves no metatable, weak-write, forwarding, active-MT or
   marking contract is involved. Active-MT paths must keep the CAS/revalidation
   helpers until an equivalent no-tear publication protocol is proved inline.
3. Restore JIT no-helper ASTORE/HSTORE for stable primitive-value stores before
   collectable-value barrier fast paths.
4. Add empty-table x64 `BC_TNEW` inline bump allocation behind strict arena,
   color, bitmap, accounting, and root-publication guards.

# GC2 sweep-boundary arena reuse

Date: 2026-07-04

GC2 mark entry now follows the planned protocol: the mark-begin handshake turns
on barriers and black allocation, but it does not request `HS_RESET_ALLOC`.
Allocation reset is owned by the sweep boundary. Moving owned arenas to
`needsweep` at mark entry made active mutators map fresh arenas for the rest of
the mark cycle, even though the old arenas were not ready for sweep yet. In
closure churn this showed up as stable per-object allocation work but growing
arena lists and increasingly expensive full collections.

`lj_gc2_sweep_prepare_bridge_boundary()` remains the place that requests
`HS_RESET_ALLOC`. The focused phase fixture now asserts that pre-mark owned
arenas remain owned through mark begin, then drives the real sweep-boundary
prepare and owner sweep before closing the synthetic cycle.

Lazy arena sweep also now republishes a replaced bump window before installing a
new one. `lj_arena_sweep_one()` reserves the largest free run in the swept arena
as the next bump window and omits that run from free-run bins. Sweeping another
arena used to overwrite the previous bump window without putting its unused
tail back into bins, stranding one large run per swept arena. The allocator now
publishes any active bump tail before replacing it or before mapping a fresh
arena because the current bump cannot satisfy a request.

The reusable-bin rebuild path uses a direct head insert while sweeping because
the bins were just cleared or are known not to contain the swept run. The
defensive duplicate scrub remains on ordinary `lj_arena_free()` and split-run
insertion, where stale or duplicate bin entries can be observed.

Focused validation:

- `tools/ci/lua_test.sh m3_gc2_scaffold`
- `tools/ci/lua_test.sh m6_jit_fnew_bump`
- `tools/ci/lua_test.sh m9_trace_hard_assist_cadence`
- `LJ_BENCH_STOCK_FILTERS=closures_upval LJ_BENCH_STOCK_SCALE=0.02 tools/ci/lua_test.sh m9_bench_stock_compare`

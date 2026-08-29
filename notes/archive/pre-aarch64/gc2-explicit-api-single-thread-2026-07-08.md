2026-07-08 explicit GC API GC2-only note
========================================

`lua_gc(LUA_GCCOLLECT)` and `collectgarbage("collect")` now route through
`lj_gc2_collect_active()` for single-thread and multi-thread executions alike.
The public API no longer selects the exclusive legacy collector just because no
secondary Lua thread is live.

`lua_gc(LUA_GCSTEP)` and `collectgarbage("step", n)` now use
`lj_gc2_step_explicit()`, a bounded GC2 state-machine step that preserves the
stock boolean completion result shape while keeping explicit user-visible GC
work on the new collector path.

Large explicit step requests are capped at 1,048,576 GC2 state-machine iterations.
This keeps the API bounded while still letting existing large-step finalizer
smokes reach the GC2 finalizer phase in one call.

GC2 sweep defers for finalizer-spawned secondary threads only when the
finalizer callback actually increases the live-thread count. A pre-existing
active peer no longer makes every SWEEP-phase explicit collect look like a
finalizer-spawn deferral.

P_WEAK FINREG cdata discovery now uses GC2 mark bits as its liveness oracle,
matching weak table clearing. Stack root validation accepts cdata through the
normal cdata/base validator before marking it, so stack-held cdata stays live
while dead ordered FINREG entries still run in reverse registration order.

The old `mt_gc_exclusive` field still exists for non-GC API synchronization
boundaries and for GC2 compatibility handoffs that have not yet been renamed or
mechanically folded into new-only helper names. The selectable public old-GC
path is gone; deleting the remaining legacy-named internals is a follow-up
cleanup once their GC2 barrier/state compatibility roles have been split out.

`tests/t-gc-stats.lua` now asserts that an explicit single-thread
`collectgarbage("collect")` advances GC2 major-cycle telemetry, and the active
thread GC fixture accepts the stock-style boolean result from repeated explicit
GC2 steps. The native channel/GC root probe keeps that specific wait loop out
of the JIT trace compiler in the JIT-mode suite run, avoiding a traced-native
reply wait from blocking the worker thread's explicit full GC completion.

Release artifacts now declare `gc64: required` and `gc: gc2-public-api` in
BUILDINFO, and the release verifier requires both fields.

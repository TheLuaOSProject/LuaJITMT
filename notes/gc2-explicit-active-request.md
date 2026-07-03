Active-thread explicit GC requests no longer disappear silently.

When `lua_gc(LUA_GCCOLLECT)` or `lua_gc(LUA_GCSTEP)` cannot claim
`mt_gc_exclusive` because secondary Lua threads are live, it now routes through
the explicit GC2 request path. These helpers use the same GC2 leader token as
allocation-triggered cycles and store the driver threshold through
`mt_gc_threshold` while `mt_live` is nonzero, so the request survives until a
GC2 owner can drive it. The automatic allocation-trigger helper stays private
to `lj_gc2.c`.

This is still a bridge. The active-thread `collect` path cannot call
`lj_gc_fullgc()` while secondary Lua threads are live; that reproduces the M4
active-child hang that the `mt_live` guard was added to prevent. Instead it
routes through `lj_gc2_collect_active()`: request or join a major GC2 cycle,
start MARK if this caller claimed an idle token, otherwise finish the current
cycle before starting a fresh major, and drive existing GC2 MARK/WEAK/SWEEP
helpers until the requested GC2 cycle returns to IDLE. This keeps the threading
safety boundary while making active
`collectgarbage("collect")` observe a completed GC2 cycle before return.
Active-thread `step` requests/assists one worker batch and returns false; this
matches the stock `step` shape without pretending to be a full collection.

Follow-up: active-thread `collectgarbage("step")` now uses an explicit request
variant that bypasses the automatic-trigger stop gate. This matches the
single-thread legacy path where `collectgarbage("step")` restarts GC after
`collectgarbage("stop")`.

Follow-up: active-thread `collectgarbage("collect")` now also bypasses the
automatic-trigger stop gate and restarts the logical GC threshold before
returning. This matches stock LuaJIT: `collectgarbage("collect")` after
`collectgarbage("stop")` leaves `collectgarbage("isrunning") == true`.

Follow-up: explicit `collect` / `step` also use the GC2 route while
`mt_entering != 0`, even if `mt_live == 0`. An entering secondary can already
own runtime transition state but has not yet published itself as live, so the
legacy single-thread collector must not claim the exclusive full-GC path in
that window. `tests/t-gc-active-collect-assist.c` now forces this state and
checks that both operations start/complete GC2 work instead.

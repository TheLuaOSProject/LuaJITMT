Active-thread explicit GC requests no longer disappear silently.

When `lua_gc(LUA_GCCOLLECT)` or `lua_gc(LUA_GCSTEP)` cannot claim
`mt_gc_exclusive` because secondary Lua threads are live, it now routes through
`lj_gc2_request_major()` / `lj_gc2_request_cycle_explicit()`. Those public
request helpers use the same GC2 leader token as allocation-triggered cycles
and store the driver threshold through `mt_gc_threshold` while `mt_live` is
nonzero, so the request survives until a GC2 owner can drive it.
The automatic allocation-trigger helper stays private to `lj_gc2.c`.

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

Stopped full `collect` records a one-shot restore bit when its request is
accepted. Because the active call now waits for GC2 IDLE,
`lj_gc2_publish_idle_threshold()` restores both the live threshold and
`mt_gc_threshold` to `LJ_MAX_MEM` before the call returns.

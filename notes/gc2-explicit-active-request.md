Active-thread explicit GC requests no longer disappear silently.

When `lua_gc(LUA_GCCOLLECT)` or `lua_gc(LUA_GCSTEP)` cannot claim
`mt_gc_exclusive` because secondary Lua threads are live, it now routes through
`lj_gc2_request_major()` / `lj_gc2_request_cycle_explicit()`. Those public
request helpers use the same GC2 leader token as allocation-triggered cycles
and store the driver threshold through `mt_gc_threshold` while `mt_live` is
nonzero, so the request survives until the legacy bridge can safely run again.
The automatic allocation-trigger helper stays private to `lj_gc2.c`.

This is still a bridge. The active-thread path performs only bounded
`lj_gc2_worker_drain()` assistance and `step` returns false; exact
`collectgarbage("collect")` parking still needs a GC2 leader path that can
close sweep without legacy collector ownership.

Follow-up: active-thread `collectgarbage("step")` now uses an explicit request
variant that bypasses the automatic-trigger stop gate. This matches the
single-thread legacy path where `collectgarbage("step")` restarts GC after
`collectgarbage("stop")`.

Stopped full `collect` now records a one-shot restore bit when its request is
accepted. The active call still returns before completion, so `isrunning` is
true while the requested cycle is pending, but `lj_gc2_publish_idle_threshold()`
restores both the live threshold and `mt_gc_threshold` to `LJ_MAX_MEM` after
that requested full cycle reaches idle.

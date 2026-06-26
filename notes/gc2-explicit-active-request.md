Active-thread explicit GC requests no longer disappear silently.

When `lua_gc(LUA_GCCOLLECT)` or `lua_gc(LUA_GCSTEP)` cannot claim
`mt_gc_exclusive` because secondary Lua threads are live, it now routes through
`lj_gc2_request_major()` / `lj_gc2_request_cycle()`. The request uses the same
GC2 leader token as allocation-triggered cycles and stores the driver threshold
through `mt_gc_threshold` while `mt_live` is nonzero, so the request survives
until the legacy bridge can safely run again.

This is still a bridge. The active-thread path performs only bounded
`lj_gc2_worker_drain()` assistance and `step` returns false; exact
`collectgarbage("collect")` parking still needs a GC2 leader path that can
close sweep without legacy collector ownership.

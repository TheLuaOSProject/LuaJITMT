GC Peer Retry Yield
===================

Internal GC/GC2 claim waits no longer sleep for a fixed 1 ms:

- legacy root traversal waits on transient table sentinels now pause/yield;
- `lua_State` GC-scan claim waits now pause/yield;
- GC2 peer/finalizer/weak-drain retry waits now pause/yield.

These helpers are not public blocking APIs. The claim protocols and owner checks
are unchanged; this only replaces coarse timer parking with a short CPU pause
and `lj_thr_yield()`, preserving native/safepoint visibility where a live
`lua_State *` is available.

`tests/t-gc2-phase.c` now samples native waits with a tight pause loop instead
of 1 ms sleeps. The previous assertion was tied to the old timer-sleep duration
and could miss the shorter native window created by yielding.

# threading.spawn root handoff

`threading.spawn()` creates the child Lua state and `threading.thread` userdata
before the OS thread can run. Those objects are published to native root lists:
the live-thread list owns the userdata, and the userdata owns startup argument
roots until `join()` clears them.

When a classic GC cycle is already active, the spawn boundary must publish this
new native root without completing the entire legacy collector. The
`lj_gc_pubobjroot()` helper builds the correctly tagged `TValue` for a `GCobj`
and reuses `lj_gc_pubroot()`, so propagation/atomic cycles see the late root
through the same barrier used by channels, callbacks, and other native roots.

GC2 is different: an active arena-mark cycle may have already snapshotted the
root set. `threading_spawn_gc_handoff()` therefore still calls
`lj_gc2_preserve_root()`, which may abort the active GC2 cycle to idle. That is
bounded cycle repair, not a legacy full collection, and the next GC2 cycle will
rescan from roots.

The legacy atomic bridge must account for that abort. If GC2 is already idle
when classic `atomic()` asks for GC2 mark completion, the bridge is complete for
this legacy cycle: classic color marking is the authoritative sweep frontier.
Retrying atomic in that state only repeats root traversal with no GC2 mark phase
left to complete.

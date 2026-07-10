# GC2 finalizer-spawn deferral latch

Finalizer callbacks may temporarily release `mt_gc_exclusive` and start a
secondary TG which outlives the callback. GC2 SWEEP must remain incomplete
until that TG exits, but this lifetime is not a legacy color-collector state.

`GC2State.finalizer_spawn_latch` now records that condition independently of
`g->gc.state`/`GCSfinalize`. It is an atomic two-bit latch: one bit identifies
the nested callback window and the other records an outliving spawned TG.

- the finalizer runner release-publishes the latch before trying to reclaim MT
  exclusion, closing the last-secondary exit race;
- a failed reclaim leaves the latch set and makes GC2 finalizer stepping defer;
- the last secondary exit observes the latch and wakes the GC2 worker;
- the GC driver consumes the latch after it observes `mt_live == 0`, then lets
  SWEEP reach its ordinary fixpoint.

Thread entry no longer interprets `GCSfinalize`. While a callback is active it
uses the callback-window bit to recover the saved logical collection threshold
from `mt_gc_threshold`. This deliberately excludes `finalizer_active`, because
that wider owner scope is also used for callback-free MPSC queue drains. Focused C and Lua
finalizer-spawn fixtures now have the spawned TG call
`collectgarbage("isrunning")`; this catches a temporary `LJ_MAX_MEM` threshold
being mistaken for a user-requested GC stop.

This is an intentional step toward the GC2-only runtime invariant. The legacy
`g->gc.state` field is neither written nor read by finalizer-spawn deferral.

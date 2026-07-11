# GC2 stale rescan dedupe and root admission

## Deterministic stale `NEEDSCAN` failure

An `rr` recording of stock test plan 86 proved that ordinary `BC_RET` and
source-v4 `BC_CGET` behavior were downstream symptoms, not the fault.  A live
test closure held a closed `GCupval` whose payload originally named the Lua
`create` closure.  The payload address was later reused by a
`string.gmatch` C closure, so the stale call returned no results and normal
return padding wrote nil to the caller's two requested result slots.

The exact loss crossed two GC2 cycles:

1. Cycle N marked the upvalue body directly, then queued and consumed its
   payload traversal.  The uncounted `LJ_GC_NEEDSCAN` dedupe bit remained set.
2. A forced or preserve-abort close retained that bare bit into IDLE.
3. Cycle N+1 directly marked the upvalue body again.  The stale bit suppressed
   its only current-cycle queue entry, so `gc2_traverse_upval()` never marked
   the `create` payload.
4. SWEEP reclaimed the payload while preserving the upvalue body.

Commit `a44d436f` clears uncounted FUNC/PROTO/UPVAL/UDATA/TRACE rescan dedupe
under the cycle-start worker token while phase is still IDLE.  TABLE and THREAD
are deliberately excluded because their `NEEDSCAN` states have counted
protocols.  `test_upval_needscan_preserve_abort()` covers both forced
`cycle_to_idle` and preserve-abort replacement cycles.

Longer term, uncounted traversal membership should be generation-tagged rather
than represented by a bare object-header bit.

## Remaining root-admission failure

The stale-dedupe repair closes that deterministic loss but does not make the
persistent root snapshot authoritative.  Further recordings show callable Lua
closures and their Protos reclaimed/reused while the stock runner is still
loading test files.  One recording reached SWEEP from `lj_parse_keepstr()` and
removed a `@test.lua:199` reader closure used by `load()`; another later
executed a wrapper through an arena body whose header and Proto storage had
already been reused.

The current dirty/freshness scheme is only a point observation.  A mutator can
publish a new frame, result range, `cur_L`, or native callback root after the
last acknowledged scan but before a phase or reclaim commit.  Rescanning the
initiating owner once on GC entry did not close the window and was discarded.

The required replacement is the planned helpable per-TG root descriptor plus
an exact OPEN/CLOSING/PENDING/COMMIT admission gate tied to the typed GC2
activation generation.  CALL/TAILCALL/RET/VARG/coroutine/native/JIT/FFI
transitions must publish ACTIVE before their first root store.  A closer must
help every active descriptor and lose its exact commit CAS to every late
publisher.  Until that coverage is complete, freshness remains a regression
guard only and cannot authorize structural reclamation by itself.

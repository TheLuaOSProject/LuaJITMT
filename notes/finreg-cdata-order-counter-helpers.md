# FINREG Cdata Ordered Counter Helpers

## Summary

Routed ordered cdata FINREG telemetry counters through helper APIs:

- `gc2_finreg_cdata_order_seen_*()`
- `gc2_finreg_cdata_order_claimed_*()`
- `gc2_finreg_cdata_order_unlinked_*()`
- `gc2_finreg_cdata_order_queued_*()`
- `gc2_finreg_cdata_order_retired_*()`
- `gc2_finreg_cdata_order_tombstones_*()`
- `gc2_finreg_cdata_order_fallbacks_*()`
- `gc2_finreg_cdata_pending_order_hits_*()`

GC2 initialization now resets these counters through relaxed helper stores.
P_WEAK ordered discovery, close-time ordered discovery, pending ordered scans,
and CTState ordered-node retirement publish increments through the helper
surface instead of ad hoc `la_add64_rlx()` calls against `g->gc2`. GC stats
export and focused FINREG fixtures now read the ordered counters through
acquire helpers.
The CTState ordered-node producer now reports retire events through
`lj_gc2_finreg_cdata_note_order_retired()`, leaving the low-level ordered
counter add behind `lj_gc2.c`.

## Guardrail

`tools/ci/m7_ffi_finreg.sh` now requires the ordered-counter helper surface and
rejects raw production access to the ordered FINREG counter fields in
`src/lj_ctype.c`, `src/lj_gc.c`, `src/lj_gc2.c`, and `src/lib_base.c`.
`tools/ci/m9_gc_stats.sh` also rejects raw FINREG ordered stat aliases.

## Follow-Up

This centralizes telemetry publication for the ordered FINREG path. Follow-up
cdata FINREG counter helper work now routes registration, clear, queue,
sweep-invariant, P_WEAK, and preclaim counters through the same style of helper
surface. Userdata FINREG counters still have direct ad hoc increments and can be
routed in a later slice.

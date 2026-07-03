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

## Coverage

`m7_ffi_finreg` owns the ordered FINREG behavior and `m9_gc_stats` owns the
public telemetry surface. Ordered FINREG counter publication must stay behind
the helper surface in `lj_ctype.c`, `lj_gc.c`, `lj_gc2.c`, and `lib_base.c`;
that rule is documented here and beside the helpers instead of in source-text
predicates.

## Follow-Up

This centralizes telemetry publication for the ordered FINREG path. Follow-up
cdata FINREG counter helper work now routes registration, clear, queue,
sweep-invariant, P_WEAK, and preclaim counters through the same style of helper
surface. Userdata FINREG counters still have direct ad hoc increments and can be
routed in a later slice.

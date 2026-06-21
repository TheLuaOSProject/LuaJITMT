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
surface instead of ad hoc `la_add64_rlx()` calls against `g->gc2`.

## Guardrail

`tools/ci/m7_ffi_finreg.sh` now requires the ordered-counter helper surface and
rejects raw production access to the ordered FINREG counter fields in
`src/lj_ctype.c`, `src/lj_gc.c`, and `src/lj_gc2.c`.

## Follow-Up

This centralizes telemetry publication for the ordered FINREG path. The broader
cdata/userdata FINREG counters still have direct ad hoc increments and can be
routed through the same style of helpers in later slices.

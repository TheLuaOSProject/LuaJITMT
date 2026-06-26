## GC2 weak scan and legacy counter helpers

Weak scan/clear telemetry tracks bounded weak snapshot scans, clear passes,
and legacy weak fallback/backfill outcomes. This slice routes
`weak_scan_runs`, `weak_scan_tables`, `weak_scan_slots`,
`weak_scan_clearable`, `weak_clear_runs`, `weak_clear_tables`,
`weak_clear_slots`, `weak_clear_cleared`, `weak_legacy_skipped`,
`weak_legacy_fallbacks`, `weak_legacy_backfills`,
`weak_legacy_backfill_tables`, `weak_legacy_backfill_slots`, and
`weak_legacy_backfill_cleared` through helper accessors in `lj_obj.h`.

Runtime initialization and increments use the helper family. GC stats export
and focused weak/phase/allocation/worker fixtures read these counters through
acquire helpers, and the M8 weak guard rejects raw production C access to
these telemetry fields.

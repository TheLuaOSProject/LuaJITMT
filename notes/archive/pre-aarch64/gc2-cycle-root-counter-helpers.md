## GC2 cycle and root-scan counter helpers

GC2 cycle scheduling and generational root-selection telemetry is updated from
allocation-triggered cycle requests, mark-begin consumption, and major/minor
root scan selection. This slice routes `cycle_requests`, `cycle_starts`,
`major_cycle_starts`, `minor_cycle_requests`, `minor_cycle_starts`,
`minor_sweep_deferred`, `minor_roots_deferred`, `major_root_scans`, and
`minor_root_scans` through helper accessors in `lj_obj.h`.

Runtime initialization and increments now use the helper family. The focused
allocation-account and traverse fixtures read the same counters through acquire
helpers, and the M6 allocation-account notes document why raw production C access to
the cycle/root telemetry fields.

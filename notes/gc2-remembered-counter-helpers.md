## GC2 remembered telemetry counter helpers

The generational remembered-set counters are updated from idle barriers,
overflow escalation, and minor-cycle SSB drain. This slice routes
`remembered_barriers`, `remembered_pushed`, `remembered_overflows`,
`remembered_filtered`, and `remembered_drained` through `gc2_remembered_*()`
helpers in `lj_obj.h`.

Runtime initialization, increments, and GC stats export now use the helper
family, and the focused alloc-account/table-store tests read the counters
through acquire helpers. The M3 worker scheduler and M9 stats guards reject raw
production C access to the remembered telemetry fields.

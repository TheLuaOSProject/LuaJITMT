## GC2 sweep-close counter helpers

The sweep-close transition counters are updated when GC2 leaves sweep for idle
and when preserve aborts return an active phase to idle. This slice routes
`sweep_to_idle` and `preserve_abort_to_idle` through helper accessors in
`lj_obj.h`.

Runtime initialization and increments now use the helper family, while the
focused phase and arena sweep tests read the counters through acquire helpers.
The M3 worker scheduler guard documents why raw production C access to the
sweep-close telemetry fields.

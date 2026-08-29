## GC2 sweep counter helpers

Sweep telemetry tracks owner-side traversable arena sweep batches, the arenas
swept with minor identity, and live cells observed after owner sweeps. This
slice routes `minor_sweep_arenas`, `sweep_owner_runs`, `sweep_owner_arenas`, and
`sweep_owner_live_cells` through helper accessors in `lj_obj.h`.

Runtime initialization and increments now use the helper family. GC stats export
and the focused allocation, worker-scheduler, and arena sweep fixtures read the
same counters through acquire helpers. The M3 worker-scheduler notes document why raw
production C access to these sweep telemetry fields.

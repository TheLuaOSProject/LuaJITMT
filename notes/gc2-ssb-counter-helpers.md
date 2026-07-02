## GC2 SSB telemetry counter helpers

The published SSB stack counters are written from mutator publication and
worker/assist drain paths. This slice routes `ssb_published`, `ssb_drained`,
`ssb_items_published`, and `ssb_items_drained` through `gc2_ssb_*()` helpers in
`lj_obj.h`.

Runtime initialization and increments now use the helper family, and the focused
C tests read the counters through the same acquire helpers. The M3 worker
scheduler guard documents why raw production C access to the counter fields.

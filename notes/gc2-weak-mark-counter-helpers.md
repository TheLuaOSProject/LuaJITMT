## GC2 weak mark counter helpers

P_WEAK barriers report first-time key and value marks through
`weak_keys_marked` and `weak_values_marked`. This slice routes those counters
through `gc2_weak_keys_marked_*` and `gc2_weak_values_marked_*` helpers.

Runtime initialization and increments now use helper accessors. GC stats and
the weak barrier fixtures read through acquire helpers, and the M8 weak guard
rejects raw production C access to these telemetry fields.

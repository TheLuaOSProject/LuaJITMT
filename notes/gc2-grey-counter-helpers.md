## GC2 grey counter helpers

Grey queue telemetry tracks objects scheduled onto and drained from the GC2 grey
work deque. This slice routes `grey_pushed` and `grey_drained` through helper
accessors in `lj_obj.h`.

Runtime initialization and increments now use the helper family, the traversal
and phase fixtures read the counters through acquire helpers, and the M3 worker
scheduler guard documents why raw production C access to the grey telemetry fields.

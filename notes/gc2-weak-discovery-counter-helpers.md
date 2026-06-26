## GC2 weak discovery counter helpers

Weak discovery telemetry tracks weak table traversal modes, successful snapshot
queueing, and overflow fallbacks. This slice routes `weak_tables_seen`,
`weak_tables_weakkey`, `weak_tables_weakval`, `weak_tables_allweak`,
`weak_tables_queued`, and `weak_tables_overflow` through helper accessors in
`lj_obj.h`.

Runtime initialization and increments now use the helper family, the traversal
fixture reads the counters through acquire helpers, and the M8 weak guard
rejects raw production C access to these discovery telemetry fields.

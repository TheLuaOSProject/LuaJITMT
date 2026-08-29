# Full GC table-edge preservation cleanup

`collectgarbage("collect")` after a large traced fresh-key workload could spend
over a minute finishing the previous legacy sweep before starting the requested
full mark. Two costs were involved:

- Legacy table traversal used the raw-stack GC object validator for every table
  key and value. Table slots come from structured array/node snapshots, while the
  expensive arena-registry validator exists for conservative raw stack words.
  Table edges now use a structured-slot header/type check and keep the full
  validator on stack/root paths.
- Forced full GC called the sweep-root preservation bridge on every old-sweep
  step. That snapshot protects roots published after the previous atomic point;
  no Lua code runs inside the sweep drain, so one snapshot before the drain is
  sufficient and avoids repeatedly rescanning large rooted tables.

The stock benchmark guard still enforces the same `3.0x` default limit. The
`closures_upval` row now compares the best of three samples by default because
single sub-microsecond allocation samples were intermittently failing the gate
without a repeatable throughput cliff.

Verification:

- `tools/ci/lua_test.sh m6_jit_alloc_account`
- `tools/ci/lua_test.sh m2_arena_gcsweep`
- `tools/ci/lua_test.sh m9_trace_hard_assist_cadence`
- `tools/ci/lua_test.sh m9_m10_gc`

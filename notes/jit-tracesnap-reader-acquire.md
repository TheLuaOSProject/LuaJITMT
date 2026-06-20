JIT tracesnap reader acquire slice

- Added acquire helpers for published snapshot arrays, snapshot maps, snapshot
  header fields, and snapshot map entries.
- Routed public `jit.util.tracesnap()` through those helpers for bounds-derived
  snapshot reads and Lua table materialization.

Validation:

- make -C src -j$(getconf _NPROCESSORS_ONLN)
- tools/ci/m5_jit_trace_publish.sh
- direct jit.util tracesnap smoke over all exits of published traces

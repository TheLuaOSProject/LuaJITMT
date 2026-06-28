2026-06-20

Slice: trace-slot publication helper documentation.

Changes:
- Historical state: `tools/ci/m5_jit_trace_publish.sh` once rejected raw
  `->trace[` indexing under `src/` before running the behavioral M5 trace
  publication suite. The source-search part was removed with the CI
  source-search cleanup; keep the rationale below as documentation.

Reasoning:
- `traceslot_pending()`, `traceslot_publish()`, and `traceslot_clear()` now
  route slot mutation through `tracevec_acq()`.
- Direct `J->trace[n]`-style slot mutation is unsafe because readers acquire
  the trace-vector snapshot and distinguish pending sentinels from published
  traces. Future changes should preserve that publication rule and cover
  observable trace behavior, not grep for helper names.
- The cached mirror assignment `J->trace = tv->slot` is not a publication
  boundary; production slot writes must still use the trace-slot helpers.

Validation:
- Use `tools/ci/lua_test.sh m5_jit_trace_publish`.

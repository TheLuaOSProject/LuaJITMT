2026-06-20

Slice: trace-slot source guard.

Changes:
- `tools/ci/m5_jit_trace_publish.sh` now rejects raw `->trace[` indexing under
  `src/` before running the behavioral M5 trace publication suite.

Reasoning:
- `traceslot_pending()`, `traceslot_publish()`, and `traceslot_clear()` now
  route slot mutation through `tracevec_acq()`.
- A narrow shell guard catches reintroduction of direct `J->trace[n]`-style
  slot mutation without trying to enforce broader trace-vector policy.
- The guard intentionally permits the cached mirror assignment
  `J->trace = tv->slot`; the production slot writes are what it protects.

Validation:
- `tools/ci/m5_jit_trace_publish.sh`

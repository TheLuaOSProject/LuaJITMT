2026-06-20

Slice: remove `J->trace` trace-slot mirror.

Changes:
- Removed `jit_State.trace`.
- Removed the remaining `J->trace = tv->slot` / `J->trace = NULL` maintenance
  from trace-vector publish/shutdown.
- Updated `tests/t-jit-tracevec.c` to assert only the RCU-published vector and
  acquired size mirror.
- Extended `tools/ci/m5_jit_trace_publish.sh` to reject reintroducing
  `GCRef *trace;` or direct `J->trace` uses under `src/`.

Reasoning:
- C trace-slot mutation now goes through `tracevec_acq()` via `traceslot_*`.
- x64 VM trace-slot loads now use `J->tracev->slot[]`.
- The old mirror was no longer a production data source; keeping it created a
  second trace-slot pointer with no independent lifetime rule.
- Removing it makes `TraceVec` the single authoritative trace slot array.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m5_jit_trace_publish.sh`
- `tools/ci/m6_jit_flush_hs.sh`

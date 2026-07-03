2026-06-20

Slice: remove `J->trace` trace-slot mirror.

Changes:
- Removed `jit_State.trace`.
- Removed the remaining `J->trace = tv->slot` / `J->trace = NULL` maintenance
  from trace-vector publish/shutdown.
- Updated `tests/t-jit-tracevec.c` to assert only the RCU-published vector and
  acquired size mirror.
- The retired mirror must not be reintroduced; that rule is documented here and
  covered through trace-vector behavior instead of source-text matching.

Reasoning:
- C trace-slot mutation now goes through `tracevec_acq()` via `traceslot_*`.
- x64 VM trace-slot loads now use `J->tracev->slot[]`.
- The old mirror was no longer a production data source; keeping it created a
  second trace-slot pointer with no independent lifetime rule.
- Removing it makes `TraceVec` the single authoritative trace slot array.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/lua_test.sh m5_jit_trace_publish`
- `tools/ci/lua_test.sh m6_jit_flush_hs`

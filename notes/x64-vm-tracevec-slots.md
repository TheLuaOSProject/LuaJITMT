2026-06-20

Slice: x64 VM trace-slot loads through `J->tracev`.

Changes:
- Added `TRACEV_SLOT_OFS` to `vm_x64.dasc`.
- x64 VM `BC_JLOOP` trace entry now loads the trace slot from
  `J->tracev->slot[traceno]` instead of the cached `J->trace` mirror.
- x64 static fallback/unpatch paths that recover `startins` for a `BC_JLOOP`
  also load the trace body from `J->tracev->slot[traceno]`.
- The trace publication docs now treat direct `J->trace`/`J_OFS(trace)` VM
  access as an obsolete pattern. The old source-search rejection was removed
  with the CI source-search cleanup; behavior and generated VM output checks own
  the observable contract.

Reasoning:
- `TraceVec` is the RCU-published vector. C `traceref()` already acquires
  `J->tracev`, bounds against the vector size, then acquire-loads the slot.
- x86_64 acquire pointer loads compile to ordinary loads, so the generated VM
  code shape stays cheap, but it now follows the published vector rather than a
  token-held mirror.
- `J->trace` remains as a cached mirror for now, but x64 runtime trace-slot
  consumers no longer depend on it.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m5_jit_trace_publish.sh`
- `tools/ci/m6_jit_flush_hs.sh`
- Reran `tools/ci/m5_jit_trace_publish.sh` after adding the new shell source
  guard.

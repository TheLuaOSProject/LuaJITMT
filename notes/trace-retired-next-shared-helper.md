2026-06-20

Slice: share retired trace/vector link helpers with GC scanning.

Changes:
- Moved `tracevec_retired_next_acq/rel()` and
  `trace_retired_next_acq/rel()` from `lj_trace.c` to `lj_jit.h`.
- Updated GC2 paranoia raw-root scanning in `lj_gc.c` to traverse retired trace
  vectors through `tracevec_retired_next_acq()`.

Reasoning:
- Retired trace vectors are shared SMR list nodes, and both trace reclamation
  and GC raw-root scanning traverse the same `retired_next` link.
- Keeping the helper in the shared JIT header avoids duplicating ad hoc
  acquire-load expressions at GC scan sites and keeps the retired-list
  synchronization vocabulary centralized.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m5_jit_trace_publish.sh`
- `tools/ci/m9_gc_stats.sh`

2026-06-20

Slice: share retired trace/vector link helpers with GC scanning.

Changes:
- Moved `tracevec_retired_next_acq/rel()` and
  `trace_retired_next_acq/rel()` from `lj_trace.c` to `lj_jit.h`.
- Updated GC2 paranoia raw-root scanning in `lj_gc.c` to traverse retired trace
  vectors through `tracevec_retired_next_acq()`.
- 2026-06-27 follow-up: added shared `tracevec_retired_head_*()` and
  `trace_retired_head_*()` helpers in `lj_jit.h` for retired-list head
  acquire/CAS/xchg operations. Runtime trace retirement, reclamation, shutdown
  freeing, mark traversal, GC2 paranoia scanning, and focused tests now use the
  shared head helpers.

Reasoning:
- Retired trace vectors are shared SMR list nodes, and both trace reclamation
  and GC raw-root scanning traverse the same `retired_next` link.
- Keeping the helper in the shared JIT header avoids duplicating ad hoc
  acquire-load expressions at GC scan sites and keeps the retired-list
  synchronization vocabulary centralized.
- Centralizing the head helpers keeps publication and teardown ordering
  documented at the same boundary as retired-list traversal instead of relying
  on raw atomics at every call site.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m5_jit_trace_publish.sh`
- `tools/ci/m9_gc_stats.sh`
- 2026-06-27 follow-up validation: `git diff --check`,
  `tools/ci/m5_jit_trace_publish.sh`, `tools/ci/m3_gc2_paranoia.sh`

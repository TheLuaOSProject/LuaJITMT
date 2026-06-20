2026-06-20

Slice: retired trace/vector next-link atomic helpers.

Changes:
- Added local `tracevec_retired_next_acq/rel()` and
  `trace_retired_next_acq/rel()` helpers in `lj_trace.c`.
- Routed trace-vector retirement push, reclaim, free, and mark traversal through
  the `TraceVec.retired_next` helpers.
- Routed trace-body retirement push, reclaim, free, mark traversal, and
  construction clears through the `GCtrace.retired_next` helpers.
- Follow-up: routed the trace-retire and trace-vector C fixtures through the
  same helpers and extended `m5_jit_trace_publish.sh` to reject raw
  `retired_next` access in those fixtures and `lj_trace.c`.

Reasoning:
- `J->retiredtracev` and `J->retiredtraces` are lockless MPSC lists published
  by CAS on the head pointer.
- The CAS release/acquire pair already made prior raw `retired_next` stores
  visible, but the link fields themselves are shared list pointers and should
  use the same acquire/release vocabulary as traversal.
- This keeps the list implementation aligned with the RCU/SMR contract without
  changing the head-CAS publication protocol.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m5_jit_trace_publish.sh`
- `tools/ci/m6_jit_flush_hs.sh`
- `tools/ci/m9_gc_stats.sh`
- Follow-up validation: `tools/ci/m5_jit_trace_publish.sh`

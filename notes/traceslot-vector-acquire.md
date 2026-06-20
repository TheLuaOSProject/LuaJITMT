2026-06-20

Slice: trace-slot writes through acquired trace vector.

Changes:
- Added `traceslot_ref_acq(J, n)` to derive a slot address from
  `tracevec_acq(J)`.
- Routed `traceslot_pending()`, `traceslot_publish()`, and `traceslot_clear()`
  through the acquired `TraceVec` instead of indexing the cached `J->trace`
  mirror directly.

Reasoning:
- `J->tracev` is the RCU-published trace-vector pointer and is release-stored
  by `tracevec_publish()`.
- Slot readers already load through `traceref()`, which acquires `J->tracev`
  and then acquire-loads the slot.
- Slot writers now also observe the current vector through the same acquire
  path before release-storing the pending/published/cleared slot state.

Intentionally left:
- `J->trace = tv->slot` remains as the existing cached mirror, and
  `t-jit-tracevec` still asserts it tracks the current vector. Production slot
  mutations no longer index through that mirror.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m5_jit_trace_publish.sh`
- `tools/ci/m6_jit_flush_hs.sh`

TG allocation counter helper surface
====================================

Status: implemented and guarded.

Changes:

- Added `lj_tg_local_total_*` helpers around `TGState.local_total`.
- Preserved the existing allocation-accounting ordering: relaxed per-TG add
  for mutator-local accumulation and acquire-release exchange when flushing
  into global GC2 pacing counters.
- Routed production allocation-accounting add/flush paths through the helper
  layer.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m6_jit_alloc_account` behavior/counter fixtures; the helper comments carry the ordering rationale.

Validation:

- `make -C src -j$(nproc)`
- `tools/ci/m6_jit_alloc_account.sh`
- `tools/ci/m5_gc2_pacing_atomic.sh`

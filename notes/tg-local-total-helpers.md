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
- Extended `tools/ci/m6_jit_alloc_account.sh` to reject raw production
  `local_total` access outside `src/lj_tg.h`.

Validation:

- `make -C src -j$(nproc)`
- `tools/ci/m6_jit_alloc_account.sh`
- `tools/ci/m5_gc2_pacing_atomic.sh`
- `tools/ci/m0_source_guard.sh`

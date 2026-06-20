2026-06-20

Slice: trace-vector release helper and shutdown clear.

Changes:
- Added `tracevec_rel(J, tv)` next to `tracevec_acq(J)`.
- `tracevec_publish()` now release-publishes the vector through
  `tracevec_rel()`.
- `lj_trace_freestate()` now acquire-loads the current vector, clears the
  cached slot mirror and size mirror, release-clears `J->tracev`, and then
  frees the saved vector pointer.

Reasoning:
- Runtime trace-vector readers and writers use `tracevec_acq()`/`tracevec_rel()`
  for the published vector pointer.
- Shutdown is quiescent, but routing it through the same helpers removes the
  last production raw `J->tracev` access outside the helper macros and avoids a
  freed pointer lingering in the JIT state during teardown diagnostics.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m5_jit_trace_publish.sh`

# Finalizer ring acquire traversal

The legacy finalizer marking loop still advanced the GC2 finalizer ring with
`gcnext()`, a raw `nextgc` load. The finalizer enqueue/drain/dequeue paths
release-publish those links, and the GC2 marker already walked the same ring
through `lj_obj_gcw_acq()`.

Changed:
- `gc_mark_finalizer_ring()` now uses `lj_obj_gcw_acq()`.
- `gc2_paranoia_check_roots()` now uses the same acquire walk for
  `finalizer_tail`.

The only remaining source `gcnext()` hit outside the macro is a C test fixture
that intentionally inspects root-list shape.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m3_gc2_paranoia.sh`
- `tools/ci/m8_weak.sh`

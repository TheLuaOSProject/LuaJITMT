GCArena next-link helpers
=========================

Slice
-----

- Added `lj_arena_next_acq()` and `lj_arena_next_rel()` in `src/lj_arena.h`
  for `GCArena.hdr.next`.
- Routed arena owned/needsweep traversal, prepare/restore/sweep splicing,
  allocator transfer, GC/GC2 arena scans, and the C fixtures that inspect or
  seed arena chains through the helpers.
- Extended `tools/ci/m3_gc2_scaffold.sh` to reject raw `GCArena.hdr.next`
  access in the production arena/GC files and the focused C fixtures.

Validation
----------

- `tools/ci/m2_arena_sweep.sh`
- `tools/ci/m3_gc2_scaffold.sh`

Notes
-----

- `TGAlloc.owned[]` and `TGAlloc.needsweep[]` remain explicit list heads.
- `LJArenaFreeRun.next` remains allocator-local free-bin state and is not part
  of this shared arena-chain helper slice.

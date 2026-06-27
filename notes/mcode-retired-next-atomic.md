## MCodeRetire next-link helpers

Slice: JIT mcode retired-record link discipline.

Changes:
- Added `mcode_retired_next_acq()` and `mcode_retired_next_rel()` in
  `src/lj_jit.h` beside `MCodeRetire`.
- Routed mcode retired-list tail walk, batch push, flush-created local chain
  construction, epoch reclaim, final free, `lj_mcode_markretired()`, and GC2
  paranoia raw-root traversal through the helpers.
- Updated `t-jit-mcode-retire` to acquire-load the retired head and traverse
  links through the helper.
- Extended `tools/ci/m6_jit_mcode_publish.sh` and
  `tools/ci/m5_jit_trace_publish.sh` to reject direct mcode retired-record
  `next` link access in the implementation, GC scan, and focused test.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m6_jit_mcode_publish.sh`
- `tools/ci/m5_jit_trace_publish.sh`
- `tools/ci/m9_gc_stats.sh`

Notes:
- Follow-up: `MCLink.next` now has separate `mcode_area_next_acq/rel()` helpers
  and `lj_mcode.c`/`lj_mcode.h` active-area chain traversals use them. This
  keeps the active mcode area chain distinct from `MCodeRetire.next` while
  still making the chain publication/acquire discipline explicit.
- `tools/ci/m6_jit_mcode_publish.sh` also rejects raw `MCLink.next` access in
  the mcode implementation and inline helpers.
- 2026-06-27 follow-up: added `mcode_retired_head_acq()`,
  `mcode_retired_head_cas()`, and `mcode_retired_head_xchg_acqrel()` in
  `src/lj_jit.h`. Mcode retired-list push, reclaim, shutdown free, marking,
  GC2 paranoia scanning, and the focused C fixture now use those helpers.
  `tools/ci/m5_jit_trace_publish.sh` and `tools/ci/m6_jit_mcode_publish.sh`
  reject raw `J->retiredmcode` access in the implementation, GC scanner, and
  focused test.

Follow-up validation:
- `git diff --check`
- `tools/ci/m6_jit_mcode_publish.sh`
- `tools/ci/m5_jit_trace_publish.sh`
- `tools/ci/m3_gc2_paranoia.sh`

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
- Documented the rule that mcode retired-record `next` is a shared
  publication link and must use the helper. Mcode retirement, GC scan, and
  focused fixtures cover behavior; CI must not enforce helper spelling by
  helper comment.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/lua_test.sh m6_jit_mcode_publish`
- `tools/ci/lua_test.sh m5_jit_trace_publish`
- `tools/ci/lua_test.sh m9_gc_stats`

Notes:
- Follow-up: `MCLink.next` now has separate `mcode_area_next_acq/rel()` helpers
  and `lj_mcode.c`/`lj_mcode.h` active-area chain traversals use them. This
  keeps the active mcode area chain distinct from `MCodeRetire.next` while
  still making the chain publication/acquire discipline explicit.
- `MCLink.next` follows the same documented helper discipline in the mcode
  implementation and inline helpers.
- 2026-06-27 follow-up: added `mcode_retired_head_acq()`,
  `mcode_retired_head_cas()`, and `mcode_retired_head_xchg_acqrel()` in
  `src/lj_jit.h`. Mcode retired-list push, reclaim, shutdown free, marking,
  GC2 paranoia scanning, and the focused C fixture now use those helpers.
  Comments beside the helper layer document the allowed `J->retiredmcode` raw
  sites; fixtures cover implementation, GC scanner, and focused-test behavior.

Follow-up validation:
- `git diff --check`
- `tools/ci/lua_test.sh m6_jit_mcode_publish`
- `tools/ci/lua_test.sh m5_jit_trace_publish`
- `tools/ci/lua_test.sh m3_gc2_paranoia`

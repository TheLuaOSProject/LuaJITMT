# x64 separated-array header sizing

Date: 2026-06-20

## Problem

The x64 VM fast paths loaded `GCtab.asize` and skipped separated-array header
loads when the mirror was zero. During array generation publication, `GCtab.array`
can already point at a separated array whose `TabArrayHdr.asize` is current
while the legacy mirror is stale low. In that window array readers/iterators can
miss the array fast path or stop traversal early.

## Fix

- Changed x64 array-size prologues to test the acquired array pointer, not the
  `GCtab.asize` mirror, before deciding whether to load `TabArrayHdr.asize`.
- Colocated arrays still use the mirror, preserving the legacy colocated path.
- Covered `ipairs_aux`, `lj_vm_next`, `TGETV/TGETB/TGETR`,
  `TSETV/TSETB/TSETR/TSETM`, and `BC_ITERN` prologues.
- Extended x64 fixtures so current separated arrays with a deliberately zeroed
  `GCtab.asize` mirror still work through TGET, `ipairs`, `pairs`/`ITERN`, and
  direct `lj_vm_next`.

## Verification

Passed:

- `tools/ci/lua_test.sh m5_x64_tget_array_header`
- `tools/ci/lua_test.sh m5_x64_ipairs_snapshot`
- `tools/ci/lua_test.sh m5_x64_itern_snapshot`
- `tools/ci/lua_test.sh m5_x64_table_next_snapshot`
- `tools/ci/lua_test.sh m5_x64_tset_nil_snapshot`
- `tools/ci/lua_test.sh m5_x64_tgets_node_order`
- `tools/ci/lua_test.sh m6_jit_aref_pair_guard`

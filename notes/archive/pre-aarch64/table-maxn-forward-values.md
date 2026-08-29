# table.maxn FORWARD filtering

Date: 2026-06-20

## Problem

`table.maxn()` raw-scanned table array and hash slots and treated any non-nil
slot as visible. That could expose the internal `FORWARD` sentinel as a real
table value:

- a bare forwarded array slot could make `table.maxn()` overcount;
- an old generation forwarded to a current array/hash generation needed to be
  resolved before deciding whether the numeric key was visible.

Other table traversal surfaces already resolve or skip forwarded slots through
the table getter and `next` forwarding paths.

## Fix

- Added local visibility helpers in `src/lib_table.c` so `table.maxn()` treats
  table-internal sentinels as invisible.
- Array slots that contain `FORWARD` now resolve through `lj_tab_getint()`.
- Numeric hash slots that contain `FORWARD` now resolve through `lj_tab_get()`
  after skipping locked internal keys.
- Extended `t-tab-forward-filter` to call the public `table.maxn()` API across:
  bare forwarded array slots, array generation hops, numeric hash hops, and
  hash-to-array forwarding.

## Verification

Passed:

- `tools/ci/lua_test.sh m5_tab_forward_filter`
- `tools/ci/lua_test.sh m6_jit_table_store_helper`

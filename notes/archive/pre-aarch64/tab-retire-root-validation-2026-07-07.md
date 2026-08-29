# Table Retire Root Validation

Date: 2026-07-07

## Problem

Retired table hash nodes and separated arrays are kept alive while still
published by any live table. The root/fixed-root scans that enforce this
conservative no-free rule read `o->gch.gct` directly from root candidates before
proving the candidate was a valid queued GC object. A stale or non-object root
candidate therefore could drive table header and side-vector loads in the
reclamation guard.

## Change

- Added `tab_gc_table_candidate()` to validate a root candidate through the GC2
  queued-object validator before reading its type tag.
- Routed `tab_gc_table_valid()`, `tab_node_still_published()`, and
  `tab_array_still_published()` through that helper before reading table
  storage pointers.
- Exposed a table-test-only helper and extended `t-tab-retire.c` to assert that
  a live table is accepted while an unregistered pointer is rejected.
- Built `m5_tab_retire` with `LJ_TAB_TEST_HELPERS`, matching the other table
  helper fixtures.

## Validation

Passed:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m5_tab_retire`
- `tools/ci/lua_test.sh m5_tab_array_publish`

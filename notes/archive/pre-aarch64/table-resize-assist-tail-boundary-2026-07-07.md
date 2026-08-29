# Table Resize Assist Tail Boundary

Date: 2026-07-07

## Problem

Writer-side array resize assist is intentionally limited to same-index slots in
the successor array. When a separated array shrinks, old tail slots must migrate
through the resize owner because tail-to-hash copy needs hash free-node
accounting and key materialization. The helper fixture covered same-index
assist, but not this negative boundary.

## Change

- Extended `t-tab-resize-copy-helper.c` with a shrinking separated-array case.
- The fixture restores a retiring old-generation tail slot, verifies the owner
  already migrated the value into the table, and asserts
  `lj_tab_test_resize_assist_array_slot()` returns `NULL` without forwarding or
  clobbering the old tail value.

## Validation

Passed:

- `tools/ci/lua_test.sh m5_tab_resize_copy_helper`
- `tools/ci/lua_test.sh m5_tab_resize_stress`

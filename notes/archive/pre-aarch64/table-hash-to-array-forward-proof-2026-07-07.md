# Table Hash-To-Array Forward Proof

This test-only slice covers the array-only successor branch for table resize
hash migration.

- `t-tab-resize-copy-helper.c` now exercises `tab_resize_copy_hash_slot()` with
  a numeric hash key whose destination table has no hash part. The copy must
  land in the array slot and preserve put-if-absent semantics if the successor
  array was already updated.
- `t-tab-forward-filter.c` now exercises `lj_tab_forwarded_hash_slot()` after a
  hash-only table grows into an array-only generation. Forwarded numeric hash
  slots must resolve through the successor array before an empty hash part can
  be treated as a miss.

Validation:

- `tools/ci/lua_test.sh m5_tab_resize_copy_helper`
- `tools/ci/lua_test.sh m5_tab_forward_filter`

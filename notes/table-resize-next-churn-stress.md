# Table resize next-churn stress

`tests/t-tab-resize-stress.lua` now includes `nextchurn`, a behavior case that
runs `next(t, nil)` and bounded `pairs(t)` observers while writer threads grow,
overwrite, and delete array and hash slots. The case verifies that concurrent
resize forwarding does not expose internal sentinel values, crash traversal, or
lose rooted object-key entries while GC steps run during the churn.

This is behavior coverage for the table forwarding protocol. It deliberately
does not search source text for helper names or field accesses; implementation
ordering requirements that are not directly observable belong in notes like
this one.

Validation:

- `tools/ci/lua_test.sh m5_tab_resize_stress`
- `git diff --check`

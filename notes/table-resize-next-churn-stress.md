# Table resize next-churn stress

`tests/t-tab-resize-stress.lua` now includes `nextchurn`, a behavior case that
runs `next(t, nil)` and bounded `pairs(t)` observers while writer threads grow,
overwrite, and delete array and hash slots. The case verifies that concurrent
resize forwarding does not expose internal sentinel values, crash traversal, or
lose rooted object-key entries while GC steps run during the churn.

The suite also includes `nextinvalid`, a semantics-boundary guard for the racy
cursor bridge: while a secondary Lua thread is live/entering, an invalid
`next()` cursor is treated as end-of-traversal so concurrent removal/clear does
not throw from another thread's stale cursor. After the secondary exits, the same
invalid cursor must again raise stock `invalid key to 'next'`.

This is behavior coverage for the table forwarding protocol. It deliberately
does not search source text for helper names or field accesses; implementation
ordering requirements that are not directly observable belong in notes like
this one.

Validation:

- `tools/ci/lua_test.sh m5_tab_resize_stress`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 LJ_M5_TAB_RESIZE_STRESS_CASES=nextinvalid tools/ci/lua_test.sh m5_tab_resize_stress`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 LJ_M5_TAB_RESIZE_STRESS_CASES=traversal,nextchurn LJ_M5_TAB_RESIZE_TRAVERSAL_MODES=pairs,next,ipairs LJ_M5_TAB_RESIZE_STRESS_REPS=1536 LJ_M5_TAB_RESIZE_STRESS_TRAVERSAL_ROUNDS=384 tools/ci/lua_test.sh m5_tab_resize_stress`
- `git diff --check`

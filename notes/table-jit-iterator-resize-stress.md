# JIT iterator resize stress

Added a `jititer` slice to `tests/t-tab-resize-stress.lua`.

The case heats iterator-heavy worker loops with JIT enabled while sibling
workers grow and prune array/hash generations. Observers exercise `pairs`,
`ipairs`, direct `next`, and stable hash/prefix reads, and only assert safety
properties that remain valid under racy table mutation: no internal sentinel
values are exposed, the stable hash anchor stays reachable, and the stable
array prefix remains intact after churn.

The shared `assert_lua_value()` diagnostic helper now tolerates a missing label
so future stress failures report the real sentinel exposure instead of failing
while formatting the message.

Validation:

- `LJ_TEST_DISABLE_BUILD_CACHE=1 LJ_M5_TAB_RESIZE_STRESS_CASES=jititer LJ_M5_TAB_RESIZE_STRESS_REPS=256 LJ_M5_TAB_RESIZE_STRESS_THREADS=3 LJ_M5_TAB_RESIZE_STRESS_TRAVERSAL_ROUNDS=96 tools/ci/lua_test.sh m5_tab_resize_stress`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m5_tab_resize_stress`

# Observe registry publication in the API handoff test

Date: 2026-09-04.

The full `t-api-gc-handoffs` fixture failed at its table-valued root hook during
`luaL_newmetatable`. The hook observed a nil anchor reservation, while its
assertion expected the registry table to have already been published.
`api_gcroot_capture_edge_reserved()` deliberately reserves nil before
admitting and transferring the live edge, so the generic anchor-push hook no
longer names the intended publication boundary.

The fixture now uses the existing new-metatable transaction hook at stage 1.
It verifies the exact registry, key, and anchor, then invokes the same
full-collection hook against the published registry slot. The exact type,
identity, before/after TValue equality, hook count, finalizer/metamethod
behavior, constructor environment, OOM, and anchor cleanup assertions remain.
No production code or hook was added.

This failure is independent of the review's GC scheduling and pre-store
barrier changes. In `/tmp/lj-api-handoff-review-09jyg7ar/`, a control archive
restored `lj_gc2.o` from `a649f737` and `lj_meta.o` from `eb77c111` (before the
barrier elision). Other objects match the combined strict assertion/helper
build. The original fixture still fails at `assert(tvistab(&before))`; the
corrected fixture passes that control and the current runtime. These are
isolated source controls, not a claim of an entire original-tree rebuild.

The fixture links with `-std=gnu11 -O2 -Wall -Wextra -Werror -mcx16`,
`LUA_USE_ASSERT`, `LJ_GC2_TEST_HELPERS`, `LJ_TAB_TEST_HELPERS`,
`LJ_TG_ROOT_TEST_HELPERS`, `LJ_API_ROOT_TEST_HELPERS`, and `-lm -ldl -pthread`.
It exits zero under a 60-second timeout and prints `t-api-gc-handoffs OK`.
The control sources, archives, stdout and stderr remain in the directory above.

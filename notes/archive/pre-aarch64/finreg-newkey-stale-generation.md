# FINREG new-key stale-generation guard

- `lj_tab_try_newkey_anchor()` and `lj_tab_try_newkey_chain()` now KEYLOCK-claim
  the destination key before installing the FINREG value claim.  That keeps a
  concurrent resize from seeing a nil key with a live claim value and dropping
  the pending registration during migration.
- Both helpers re-check that their sampled hash generation is still current
  after reserving a free slot, after claiming a key, and before publishing a
  linked collision node.  If a resize wins, they abandon the local claim and
  return `-1` so the caller re-enters lookup/new-generation handling.
- Successful helper inserts return `lj_tab_set(L, t, key)` after publishing the
  real key.  This routes the returned value slot through any FORWARD installed
  by a concurrent resize while the KEYLOCK/claim pair was being migrated.
- Added `LJ_TAB_TEST_HELPERS` hooks for the post-reserve window and
  `tests/t-tab-finreg-newkey-stale.c`, which injects an actual resize after
  the reserve in both the anchor and collision-chain helper paths.
- Added a `tablelib` resize stress case that drives public `table.insert`
  shifting while other threads churn hash resizes, then verifies inserted
  marker objects remain reachable after full GC.

Validation:

- `tools/ci/lua_test.sh m5_tab_finreg_newkey_stale`
- `tools/ci/lua_test.sh m5_tab_keylock_lookup`
- `LJ_M5_TAB_RESIZE_STRESS_CASES=tablelib LJ_M5_TAB_RESIZE_STRESS_REPS=192 LJ_M5_TAB_RESIZE_STRESS_THREADS=2 tools/ci/lua_test.sh m5_tab_resize_stress`
- `tools/ci/lua_test.sh m5_tab_cas_store`
- `tools/ci/lua_test.sh m7_ffi_finreg`

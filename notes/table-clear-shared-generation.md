# table.clear with shared table generations

- `table.clear` used to call `lj_tab_clear(GCtab *)`, which raw-nilled the
  array part and destructively cleared hash `key`, `val`, and `next` fields.
  That is safe for private tables, but it violates the table-generation rule
  once secondary Lua threads have existed: hash nodes are not moved or unlinked
  within a published generation.
- `lj_tab_clear(lua_State *, GCtab *)` now keeps the raw clear path when
  `mt_active` is still zero. After threading activation it enters the table
  structural token, clears array and hash values through current-slot/keyed CAS,
  and leaves keys/chains intact as dead-key style tombstones.
- The shared path waits out transient key locks and FFI finalizer publication
  claims, and does not overwrite `FORWARD` markers. JIT-recorded `table.clear`
  now calls the same helper with the implicit `lua_State *`, so traces compiled
  before threading activation still take the shared path at runtime after
  `mt_active` flips.
- Coverage: `tests/t-tab-resize-stress.lua` now has a `tableclear` case that
  runs `table.clear` alongside hash growth, traversal, GC steps, and a final
  post-clear marker retention check.

Verification:

- `make clean && make -j$(nproc)`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 LJ_M5_TAB_RESIZE_STRESS_CASES=tableclear LJ_M5_TAB_RESIZE_STRESS_REPS=192 LJ_M5_TAB_RESIZE_STRESS_THREADS=2 LJ_M5_TAB_RESIZE_STRESS_TRAVERSAL_ROUNDS=64 tools/ci/lua_test.sh m5_tab_resize_stress`

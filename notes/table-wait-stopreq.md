# Table Retry Wait STOPREQ Freshness

- `lj_tab_wait_l()` still uses the shared spin/yield retry primitive for short
  KEYLOCK, FORWARD, and structural-owner contention.
- Lua-state table waits now snapshot whether `TGF_STOPREQ` was already sticky
  before yielding, then apply the fresh STOPREQ check after native leave. A
  pre-existing sticky STOPREQ does not interrupt an otherwise successful table
  retry, but a STOPREQ delivered while the wait is native-visible is raised at
  the wait boundary.
- The no-`lua_State` helper remains a yield-only retry path because it cannot
  throw through a protected Lua frame.

Coverage: `m5_tab_struct_owner` exercises sticky and fresh STOPREQ behavior via
the exported table retry wait helper under `LJ_TAB_TEST_HELPERS`.

# C Wrapper Callback Publication

`luaJIT_setmode(..., LUAJIT_MODE_WRAPCFUNC|LUAJIT_MODE_ON)` publishes the
universe-global C wrapper callback and then switches external C function calls
from `BC_FUNCC` to `BC_FUNCCW`.

The callback is now release-stored through `wrapf_store()` before publishing
`BC_FUNCCW`. This preserves the existing generated x64 VM load shape while
removing the raw C-side data race on `global_State.wrapf`.

Guard:

- `m5_wrapcf_func_publish` rejects raw C-side `->wrapf` access outside the
  accessor definition and verifies that `luaJIT_setmode()` stores the wrapper
  before publishing `BC_FUNCCW`.

Validation:

- `make -C src -j$(nproc)`
- `tools/ci/lua_test.sh m5_wrapcf_func_publish m5_panic_callback_atomic m5_hook_state_atomic`
- `tools/ci/lua_test.sh run_stock_tests -- --quiet`
- `git diff --check`

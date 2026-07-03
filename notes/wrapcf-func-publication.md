# C Wrapper Callback Publication

`luaJIT_setmode(..., LUAJIT_MODE_WRAPCFUNC|LUAJIT_MODE_ON)` publishes the
universe-global C wrapper callback and then switches external C function calls
from `BC_FUNCC` to `BC_FUNCCW`.

The callback is now read from the target stack while holding a state claim, then
release-stored through `wrapf_store()` before publishing `BC_FUNCCW`. This
preserves the existing generated x64 VM load shape while removing the raw C-side
data race on `global_State.wrapf`.

Coverage:

- `m5_wrapcf_func_publish` documents why raw C-side `->wrapf` access outside the
  accessor definition and verifies that `luaJIT_setmode()` claims the target
  stack slot, stores the wrapper, drops the claim, and then publishes
  `BC_FUNCCW`.

Validation:

- `make -C src -j$(nproc)`
- `tools/ci/lua_test.sh m5_wrapcf_func_publish m5_panic_callback_atomic m5_hook_state_atomic`
- `tools/ci/lua_test.sh run_stock_tests -- --quiet`
- `git diff --check`

Follow-up:

- `luaJIT_setmode()` FUNC/ALLFUNC/ALLSUBFUNC modes now acquire the JIT token,
  claim the target state while resolving the function/prototype slot and
  mutating prototype JIT flags, drop the claim, and only then publish the trace
  flush boundary.

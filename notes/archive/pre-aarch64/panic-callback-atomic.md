# Panic Callback Atomics

`global_State.panic` is a universe-global public C API callback and can be
changed through `lua_atpanic()`. It now follows the hook callback publication
model instead of raw field access:

- `panicf_load()` acquire-loads the callback before fatal error paths invoke it.
- `panicf_store()` release-publishes the default auxiliary-library panic
  handler during state creation.
- `panicf_xchg()` atomically exchanges the callback for `lua_atpanic()`, so
  concurrent callers receive a real previous value instead of racing a plain
  load/store pair.
- The M5 publication suite documents why raw target-runtime `->panic` access outside
  these accessor definitions and checks the stock `lua_atpanic()` exchange
  contract with a C fixture.

Validation:

- `make -C src -j$(nproc)`
- `tools/ci/lua_test.sh m5_panic_callback_atomic m5_hook_state_atomic m5_hookmask_atomic m5_stock_api_surface`
- `tools/ci/lua_test.sh run_stock_tests -- --quiet`
- `git diff --check`

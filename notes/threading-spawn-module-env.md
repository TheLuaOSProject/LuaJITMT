# Threading spawn module environment

`m6_jit_token` exposed a deterministic failure in `tests/t-jit-secondary.lua`:
the spawned worker could call `require("jit_harness")`, but the loaded
`jit_harness.lua` chunk saw global `require == nil`.

The root cause was environment selection in `threading.spawn()`. It called
`lua_newthread()` while executing the `threading.spawn` C function, so
`lua_newthread()` inherited the library C function environment. For threading
library functions this can be a method/module table, not the user function's
global environment. File modules loaded by `require()` then used that wrong
thread environment.

`threading_spawn_core()` now retargets the child Lua state environment to the
caller thread environment. This matches stock LuaJIT coroutine behavior: the
new thread's `getfenv(0)` inherits the caller thread environment, while the
spawned function still uses its own function environment for globals.

Regression coverage:

- `tests/t-threading-api.lua` now requires a file-backed module from inside a
  spawned thread. The module itself calls `require("threading")`, so it fails if
  the loaded chunk does not see a proper global environment.
- The same test checks that a spawned thread inherits the caller thread
  environment separately from the spawned function environment, matching stock
  coroutine behavior.

Verification:

- `tools/ci/lua_test.sh m4_threading_api`
- `tools/ci/lua_test.sh m6_jit_token`
- `tools/ci/lua_test.sh m6_jit`
- `tools/ci/lua_test.sh run_stock_tests -- --quiet`

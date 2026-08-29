2026-06-20

Subject: Lua-owned cross-suite dispatch.

`suite_runtime.run_lua_test_case()` and `run_lua_test_cases()` used to spawn
`tools/ci/lua_test.sh`, which meant aggregate Lua suites re-entered the suite
through a shell compatibility launcher. They now call `utils.run_case()` against
the Lua registry loaded from `tests/suites/init.lua`.

This keeps `tools/ci/*.sh` as external compatibility entrypoints while the test
suite's internal composition stays in Lua. It also avoids nested shell process
setup for aggregate cases such as `m5_concurrent_objects`, `m6_jit`, and
`m7_ffi`.

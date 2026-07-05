# Test Build Cache Freshness

Date: 2026-07-05

`Test:build({ clean = true })` used to skip rebuilding whenever the cached
build profile signature matched and `src/luajit` plus `src/libluajit.a`
existed. The signature only covered build flags, so local source edits could
leave tests and stock comparisons running a stale binary unless
`LJ_TEST_DISABLE_BUILD_CACHE=1` was set.

The cache now also checks tracked `src/` and `dynasm/` build inputs. A cached
clean build is reused only when no tracked input is newer than either primary
output. Untracked scratch files are ignored, which keeps local notes/probes from
invalidating every test run.

Validation:

- `LUA=luajit tools/ci/lua_test.sh m9_bench_regression` skipped the build when
  outputs were current.
- After `touch src/lj_func.c`, the same command rebuilt LuaJIT before running
  the regression check.

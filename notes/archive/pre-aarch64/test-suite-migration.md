# Test Suite Migration

The active test harness is Lua-owned:

- `tools/ci/lua_test.sh <case...>` is the canonical local and CI entry point.
- `tools/test.lua` dispatches named cases from `tests/suites/*.lua`.
- `tests/lib/ljtest.lua` owns command execution, VM builds, C fixture builds,
  stock-suite invocation, and LuaJIT process launch.
- Shell under `tools/ci/` is reserved for real platform orchestration, not for
  pure case aliases.

Coverage must exercise behavior or product artifacts:

- Lua and C fixtures cover language/runtime behavior.
- Runtime counters cover internal progress that has an observable API.
- Stock tests cover stock Lua/LuaJIT semantics.
- Benchmark CSVs, release manifests, install trees, process output, and opaque
  bytecode load/execute payloads are product artifacts and may be checked as
  text or bytes.

Repository implementation files are not test artifacts. The suite must not read
`src/`, `dynasm/`, generated VM text, or JIT/ASM dump output merely to assert a
helper name, instruction spelling/count, old wrapper name, or forbidden string.
When an implementation constraint is not directly observable, document it in a
local code comment or a focused note. When it is observable, add a fixture,
counter, stock-semantics check, benchmark check, or release/package artifact
check.

Current helper boundaries:

- `suite_utils.lua` keeps generic command, path, temp-file, and public-artifact
  helpers.
- `suite_assert.lua` owns text assertions for process output and test-owned
  artifacts only.
- `suite_runtime.lua` owns LuaJIT subprocess, stock-suite, and aggregate runner
  plumbing.
- `suite_build.lua` owns C fixture compilation/link/run helpers.

Build-owning cases are still serialized per worktree by the runner lock because
many cases rebuild `src/` with different flags. Use separate worktrees for
parallel validation when build profiles differ.

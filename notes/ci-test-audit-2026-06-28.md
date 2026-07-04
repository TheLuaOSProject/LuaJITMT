# CI/Test Harness Audit - 2026-06-28

Current guidance for tests and CI:

- `tools/ci/lua_test.sh <case...>` is the canonical way to run named suite
  cases.
- `tools/ci/platform_build.sh` owns platform build smoke.
- Release packaging validation lives in `tests/suites/release.lua` and
  `tools/release/verify_artifacts.sh`.
- A shell script should remain only when it performs real platform setup,
  release orchestration, or non-suite work.

Implementation-only rules are documented beside the constrained code or in a
focused note. Tests should cover behavior, counters, public artifacts, release
metadata, benchmarks, packaging, stock semantics, or process output.

## Current Harness Shape

- `tests/lib/ljtest.lua` caches repeated same-profile clean builds in one
  runner process and persists build-profile signatures across processes.
- `tests/lib/suite_build.lua` owns LuaJIT-linked and standalone C fixture
  compilation.
- `tests/lib/suite_runtime.lua` owns stock-suite, LuaJIT subprocess, and
  aggregate-case runner plumbing.
- `tests/lib/suite_assert.lua` owns assertions over process output and
  test-owned artifacts.

## Operational Notes

- Build-owning tests are serialized by the runner lock in one checkout.
- Use separate worktrees for parallel runs with different build profiles.
- Keep stock LuaJIT API compatibility checks in behavior/C fixtures; do not
  remove public stock symbols merely because they are not part of the threading
  experiment.

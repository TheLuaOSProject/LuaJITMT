# Progress Report - 2026-06-28

This note keeps the durable state from the 2026-06-28 cleanup work without
preserving obsolete implementation-text cleanup records. Historical per-slice
details remain available in git history; current HEAD keeps only the rules and
outcomes that still guide work.

## Project State

- Branch focus: x86_64 Linux first, with macOS and Windows release/build
  coverage maintained through the platform harness.
- Priority order: stock Lua/LuaJIT semantics, memory safety, GC visibility,
  stability, then performance.
- The Lua test harness is the canonical test surface. Pure shell aliases and
  implementation-text checks are not active coverage.

## Landed Runtime Areas

- Table resize forwarding and publication: array/hash generations publish
  successor links, retiring generations stay visible until safe, and GC/weak
  traversal resolves forwarded slots.
- X64 closed-upvalue stores publish full `TValue` data through the runtime
  helper path.
- GC2 telemetry and ownership helpers expose behavior through counters and
  stats snapshots.
- FFI/native-state work has focused fixtures for parser token waits,
  callbacks, cdata access, finalizer registration, and STOPREQ delivery.

## Harness State

- Use `tools/ci/lua_test.sh <case...>` for focused local validation.
- Platform build smoke lives in `tools/ci/platform_build.sh`.
- Release package checks live in `tests/suites/release.lua` and
  `tools/release/verify_artifacts.sh`.
- Build-owning tests should run serially in one worktree; use additional
  worktrees for parallel build-profile validation.

## Coverage Rule

Implementation-only constraints belong beside the constrained code or in a
focused design note. Observable failures must be covered with Lua/C behavior
fixtures, runtime counters, stock tests, benchmark comparisons, process output,
or release/package artifacts. Current tests should not inspect repository
implementation text as a substitute for those signals.

## Current Risks

- FFI parser fallback paths still need careful snapshot/refetch cleanup outside
  mutable `ffi.cdef`.
- Table resize and weak/finalizer interactions remain high-value stress areas.
- GC2 pacing and assist overhead still need performance work after correctness
  is stable.
- Release confidence still depends on local Linux, Wine/Windows, and
  Darling/macOS smoke before publishing artifacts.

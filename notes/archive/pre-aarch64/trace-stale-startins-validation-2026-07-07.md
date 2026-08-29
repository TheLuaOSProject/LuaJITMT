# Trace Stale Startins Validation

Date: 2026-07-07

## Problem

`lj_trace_stale_startins()` is a fallback path for recovering the original
bytecode instruction from live, retired, or legacy-rooted trace bodies. Its
root-spine scan still read `o->gch.gct` before proving that the root candidate
was a valid trace object, and the trace-vector and retired-list scans read
`startpc` and `startins` before rechecking the compact trace-body layout.

## Change

- Added `trace_stale_startins_match_valid()` so live and retired trace fallback
  searches validate the compact body before reading stale-startins metadata.
- Added `trace_stale_startins_root_candidate()` so legacy-root fallback rejects
  non-trace and invalid root candidates before trace-field reads.
- Switched the trace-vector fallback search to `traceref_safe()` before the
  compact-body validator.
- Extended the trace-retire fixture with a valid empty-IR compact trace body and
  assertions for valid trace, proto, and invalid-pointer stale-startins
  candidates.

## Validation

Passed:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m5_jit_trace_publish`
- `make -C src clean`
- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m6_jit_flush_hs`
- `tools/ci/lua_test.sh m5_bcdump_compat`
- `git diff --check`

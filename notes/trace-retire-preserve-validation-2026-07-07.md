# Trace Retire Preserve Validation

Date: 2026-07-07

## Problem

The retired-trace preservation path could inspect root-spine and trace-body
candidates before proving that the candidate was still a valid queued GC
object. In particular, trace retirement could read `o->gch.gct`,
`proto_bc(pt)`, and `pt->sizebc` while trying to preserve trace body objects
and stale snapshot-PC proto owners.

## Change

- Added `trace_preserve_body_candidate()` so retired trace body preservation
  validates the candidate object before reading its type tag.
- Added `trace_proto_pc_candidate()` so snapshot-PC proto preservation validates
  the root object and proto traversal shape before caching or range-checking the
  proto bytecode interval.
- Exported the existing GC2 proto traversal validator for the trace
  preservation path.
- Added trace-retire fixture assertions for valid proto candidates, exclusive
  bytecode-end handling, and an invalid pointer that must be rejected before any
  header read.
- Built the trace publication fixture with `LJ_TRACE_TEST_HELPERS` for both the
  library and C fixture.

## Validation

Passed:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m5_jit_trace_publish`
- `make -C src clean`
- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m6_jit_trace_proto_gc`
- direct `tests/t-jit-mcode-fresh.lua` under the CI `LUA_PATH` with
  `timeout 120s`
- `M6_MCODE_TIMEOUT=120s tools/ci/lua_test.sh m6_jit_mcode_publish`
- `tools/ci/lua_test.sh m9_m10_gc`
- `git diff --check`

A first `tools/ci/lua_test.sh m6_jit_mcode_publish` run with the default
60-second fixture timeout timed out in `tests/t-jit-mcode-fresh.lua`. A later
120-second wrapper run hit the previously observed transient channel-method
lookup in the same fixture. The direct fixture command passed before and after,
and the 120-second wrapper rerun passed.

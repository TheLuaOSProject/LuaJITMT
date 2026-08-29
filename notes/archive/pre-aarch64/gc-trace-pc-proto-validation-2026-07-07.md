# Legacy GC Trace PC Proto Validation

Date: 2026-07-07

## Problem

The legacy GC trace traversal path preserves prototype owners for snapshot PCs.
Its root-spine scan still read `o->gch.gct`, `proto_bc(pt)`, and `pt->sizebc`
from candidate roots before proving that the candidate was a valid proto
object. GC2 and retired-trace preservation already had guarded candidate paths;
the classic GC traversal path needed the same protection.

## Change

- Added `gc_trace_pc_proto_candidate()` to validate a legacy root-spine object
  and proto traversal shape before reading the proto bytecode interval.
- Routed `gc_mark_proto_for_trace_pc()` through that candidate helper before
  caching or marking proto owners.
- Added a legacy-GC test helper and reused the existing GC2 traversal fixture to
  assert valid proto/PC, exclusive bytecode-end, and invalid-pointer rejection.
- Exposed the helper for both `LJ_GC2_TEST_HELPERS` and the paranoia/assert
  fixture variants, matching the existing `lj_gc2_test_*` helper policy.

## Validation

Passed:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m3_gc2_scaffold`
- `tools/ci/lua_test.sh m6_jit_trace_proto_gc`
- `git diff --check`

The first `m3_gc2_scaffold` attempt caught the helper exposure mismatch for the
paranoia fixture variant. After updating the declaration condition, the full
target passed, including `m3_gc2_paranoia`, `m2_arena_all`, and `m0_matrix`.

# GC2 Trace PC Proto Validation

Date: 2026-07-07

## Problem

`gc2_mark_proto_for_trace_pc_root()` and the worker-side scan walked the
legacy root spine for JIT snapshot PCs and read `o->gch.gct`, `proto_bc(pt)`,
and `pt->sizebc` before validating that the root-spine entry was still a live
proto object. A stale root-spine entry could therefore be dereferenced while
GC2 was trying to preserve a proto for a trace PC.

## Change

- Added `gc2_trace_pc_proto_candidate()` to validate the root object base and
  proto traversal shape before doing the snapshot-PC range check.
- Routed both owner and worker trace-PC proto scans through the shared
  validator.
- Added a GC2 test helper and assertions for:
  - a valid proto/PC pair,
  - the exclusive bytecode-end boundary,
  - an invalid candidate pointer that must be rejected before any proto read.
- Fixed `tests/t-jit-util-flush-race.lua` to use the established
  `worker:join(20)` userdata method form, allowing the target to reach its
  intended join/result assertions.

## Validation

Passed:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `cc -std=gnu11 -O2 -Wall -Wextra -Werror -mcx16 -DLJ_GC2_TEST_HELPERS -I src tests/t-gc2-traverse.c src/libluajit.a -lm -ldl -pthread -o /tmp/lj_t-gc2-traverse && /tmp/lj_t-gc2-traverse`
- `tools/ci/lua_test.sh m6_jit_util_flush_race`
- `tools/ci/lua_test.sh m6_jit_mcode_publish`
- `tools/ci/lua_test.sh m6_jit_flush_thread_stress`
- `tools/ci/lua_test.sh m3_gc2_scaffold`
- `tools/ci/lua_test.sh m9_m10_gc`
- `git diff --check`

Aggregate `tools/ci/lua_test.sh m6_jit` was also exercised. After the
flush-race join fix it passed the trace/proto GC, GC2 readiness, mcode publish,
and patched flush-race portions, but two full aggregate attempts failed late in
unrelated stress targets: one transient `t-jit-mcode-fresh.lua` channel method
lookup that passed in isolated `m6_jit_mcode_publish`, and one
`m6_jit_flush_thread_stress` timeout that passed immediately in isolation.

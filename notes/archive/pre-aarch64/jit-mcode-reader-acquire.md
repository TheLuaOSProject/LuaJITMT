JIT mcode reader acquire slice

- Added acquire helpers for published trace mcode fields: mcode, exitstub,
  szmcode, and mcloop.
- Routed `jit.util.tracemc()` through acquired mcode metadata before exposing
  executable-memory contents and returning address/loop metadata.
- Routed `jit.util.traceexitstub()` through an acquired local mcode view before
  applying the target-specific exit-stub address macro.
- Guarded x64 per-trace exit-stub address arithmetic when a trace has mcode but
  no per-trace exitstub.

Validation:

- make -C src -j$(getconf _NPROCESSORS_ONLN)
- tools/ci/m5_jit_trace_publish.sh
- direct jit.util tracemc/traceexitstub smoke
- direct small-mcode tracemc stress smoke
- direct t-jit-mcode-fresh.lua x3 with CI LUA_PATH
- tools/ci/m6_jit_mcode_publish.sh

2026-07-03 follow-up:

- Active `m6_jit_mcode_publish` no longer inspects generated exit-stub encoding
  or asserts a specific x64 jump form. That route is documented beside the
  relevant backend code.
- The durable invariant is that published mcode metadata is acquired before
  readers use it, mcode protection remains W^X/execute-stable where the target
  requires it, and traces continue to execute under small-mcode pressure. Those
  parts are covered by `t-jit-mcode-prot.c`, runtime traceability checks, and
  `t-jit-mcode-fresh.lua`.

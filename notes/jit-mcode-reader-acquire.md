JIT mcode reader acquire slice

- Added acquire helpers for published trace mcode fields: mcode, exitstub,
  szmcode, and mcloop.
- Routed `jit.util.tracemc()` through acquired mcode metadata before copying
  machine code bytes and returning address/loop metadata.
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

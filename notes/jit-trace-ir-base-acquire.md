JIT trace IR base acquire slice

- Added `trace_ir_acq()` for published trace IR-array pointers.
- Routed public `jit.util.traceir()` and `jit.util.tracek()` through the
  acquired IR base before loading IR instruction snapshots.

Validation:

- make -C src -j$(getconf _NPROCESSORS_ONLN)
- tools/ci/m5_jit_trace_publish.sh
- direct jit.util traceir/tracek smoke

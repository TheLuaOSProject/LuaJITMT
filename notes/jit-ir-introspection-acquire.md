JIT IR introspection acquire slice

- Added `ir_load_acq()` to acquire-load a published 64-bit `IRIns` word as
  a coherent snapshot.
- Used the snapshot for public `jit.util.traceir()` output fields.
- Used the snapshot for `jit.util.tracek()` KSLOT resolution, FFI ctype setup,
  and returned IR type, while leaving constant value decoding on the original
  trace pointer so second-slot constants still read from real trace storage.

Validation:

- make -C src -j$(getconf _NPROCESSORS_ONLN)
- tools/ci/m5_jit_trace_publish.sh
- ./src/luajit tests/stock/test/misc/gc_trace.lua
- direct jit.util traceinfo/traceir/tracek smoke
- tools/ci/m6_jit.sh

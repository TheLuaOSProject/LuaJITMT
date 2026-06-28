Trace debug/export acquire slice
================================

Changes:
- Added acquire helpers for trace start PC, original start instruction, stack
  adjustment, and optional GDB JIT entry publication.
- Updated bytecode dump unpatching to copy acquired trace start instructions.
- Updated debug frame PC correction and root unpatching to use acquired trace
  start metadata.
- Updated optional GDB JIT and perf-map trace export paths to snapshot acquired
  trace metadata after publication.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `make -C src clean && make -C src -j$(getconf _NPROCESSORS_ONLN) XCFLAGS='-DLUAJIT_USE_GDBJIT -DLUAJIT_USE_PERFTOOLS'`
- `make -C src clean && make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m6_jit_flush_hs.sh`
- `tools/ci/lua_test.sh m5_bcdump_current`
- `tools/ci/m6_jit_mcode_publish.sh`

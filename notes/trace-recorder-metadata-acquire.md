Trace recorder metadata acquire slice
=====================================

Changes:
- Added acquire helpers for snapshot exit counts and root trace child counts.
- Updated compiled-function recording to acquire trace link type and original
  start instruction before temporarily unpatching JFUNC bytecode.
- Updated JIT loop recording to snapshot linked trace start instructions once.
- Updated side-trace setup throttling to acquire root, snapshot metadata, child
  count, and hot-exit count from published parent/root traces.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m6_jit.sh`

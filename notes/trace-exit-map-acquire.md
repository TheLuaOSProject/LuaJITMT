Trace exit map acquire slice
============================

Changes:
- Added `snap_mcofs_acq()` for snapshot mcode offset reads.
- Updated trace exit PC lookup to acquire `mcode`/`szmcode` before matching a PC.
- Updated stack-check exit remapping to acquire trace snapshot count, root, and
  REF_BASE IR metadata.
- Updated JIT unwind exit mapping to snapshot acquired mcode/snapshot metadata
  and pass an acquired mcode view into per-trace exit-stub address helpers.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m6_jit_flush_hs.sh`
- Direct rerun after one wrapper timeout:
  `LUA_PATH='/workspaces/lj-lockless/tests/lib/?.lua;/workspaces/lj-lockless/src/?.lua;/workspaces/lj-lockless/src/jit/?.lua;;' timeout 120s ./src/luajit tests/t-jit-mcode-fresh.lua`
- `tools/ci/m6_jit_mcode_publish.sh`

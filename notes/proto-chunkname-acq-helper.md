# Prototype chunkname acquire helper

2026-06-20

- Added `proto_chunkname_acq()` and `proto_chunknamestr_acq()` next to the raw
  local macros. Parser and bytecode-reader construction already release-publish
  `GCproto.chunkname`.
- Routed published readers through the acquire helpers:
  `jit.util.funcinfo()`, bytecode dump header writing, debug location/source
  reporting, legacy GC prototype marking, GC2 prototype marking, GDB JIT
  filenames, and perf-map trace names.
- Updated the GC2 traversal fixture to validate the release-published chunkname
  through the same acquire helper.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m5_proto_chunkname_acq` behavior/counter fixtures; the helper comments carry the ordering rationale.

Validation:
- `tools/ci/m5_proto_chunkname_acq.sh`
- `tools/ci/lua_test.sh m3_gc2_paranoia m5_jit_trace_publish`

# Prototype KGC acquire helper

2026-06-20

- Added `proto_kgc_acq()` next to `proto_kgc()` so readers of
  release-published prototype GC constants can name the acquire edge.
- Routed non-recorder/runtime readers through it:
  `jit.util.funck()`, debug slot-name lookup, JIT mode child-prototype walks,
  legacy GC prototype marking, GC2 prototype marking, and FFI equality
  metamethod string constants.
- Left `lj_record.c` on `proto_kgc()`: those uses are recorder-owned current
  prototype reads and stay with the existing JIT ownership model.
- Documented the invariant formerly checked by `m5_proto_kgc_acq`: raw `proto_kgc()` use in the
  converted published-reader files.

Validation:
- `tools/ci/m5_proto_kgc_acq.sh`
- `tools/ci/lua_test.sh m5_jit_trace_publish m3_gc2_paranoia`

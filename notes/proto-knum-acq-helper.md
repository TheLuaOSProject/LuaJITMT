# Prototype numeric constant acquire helper

2026-06-20

- Added `proto_knumtv_load_acq()` for release-published prototype numeric
  constants stored in the shared `GCproto.k` TValue area.
- Routed public/runtime readers through it:
  `jit.util.funck()` positive-index constants, bytecode dump numeric constant
  writing, and the FFI cdata equality path for `BC_ISEQN`.
- Left recorder-owned `proto_knumtv()` reads in `lj_record.c` unchanged; those
  operate under the current recorder ownership model and are handled separately
  from published-prototype readers.
- Added `tools/ci/m5_proto_knum_acq.sh` to reject raw numeric-constant reads in
  the converted files.

Validation:
- `tools/ci/m5_proto_knum_acq.sh`
- `tools/ci/lua_test.sh m5_proto_kgc_acq m5_proto_chunkname_acq m7_ffi_metatype`

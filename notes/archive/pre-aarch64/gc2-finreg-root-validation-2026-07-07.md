# GC2 FINREG/root validation hardening

This slice closes two stale-header-read paths found while hardening FINREG:

- FINREG cdata/userdata entry points now validate the queued object memory
  before reading `gct`. Invalid public/test entry calls return without touching
  FINREG counters or finalizer queues.
- Root/TValue publication now preflights collectable values before legacy color
  reads. Strings use `lj_gc2_mem_registered_known()` because string objects are
  plain allocations and may not be aligned like traversable GC-object cells.

The first `m7_ffi_finreg` rerun exposed stale string roots during FFI stack
publication. The final fix keeps FNEW capture semantics unchanged and instead
teaches the root/GC2 TValue validators to accept live strings via allocation
membership before reading the string header.

Validation:

- `tools/ci/lua_test.sh m3_safepoint_handshake`
- direct `/tmp/lj_t_safepoint_handshake` stress, 10 consecutive runs
- `tools/ci/lua_test.sh m7_ffi_finreg`
- `tools/ci/lua_test.sh m7_ffi`
- `tools/ci/lua_test.sh m9_m10_gc`

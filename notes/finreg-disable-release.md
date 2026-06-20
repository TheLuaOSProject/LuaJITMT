FINREG disable release-store slice

- `lj_gc_finalize_cdata_disable()` now clears each FINREG generation
  metatable with `setgcrefnullrel()`.
- FINREG readers already test generation liveness with acquire loads of
  `t->metatable`; disabling now matches that publication protocol.

Verification:

- tools/ci/lua_test.sh m7_ffi_finreg
- tools/ci/lua_test.sh m8_weak

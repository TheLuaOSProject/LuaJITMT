FINREG disable release-store slice

- `lj_gc2_finreg_cdata_disable()` now owns the close-time FINREG generation
  walk and clears each FINREG generation table with `fin_gen_tab_disable_rel()`;
  a later cleanup removed the legacy close wrapper, so `lua_close()` calls the
  GC2 FINREG disable helper directly.
- FINREG readers already test generation liveness with acquire loads of
  `t->metatable`; disabling now matches that publication protocol.

Verification:

- tools/ci/lua_test.sh m7_ffi_finreg
- tools/ci/lua_test.sh m8_weak

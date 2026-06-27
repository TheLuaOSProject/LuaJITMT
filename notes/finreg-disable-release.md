FINREG disable release-store slice

- `lj_gc2_finreg_cdata_disable()` now owns the close-time FINREG generation
  walk and clears each FINREG generation table with `fin_gen_tab_disable_rel()`;
  `lj_gc_finalize_cdata_disable()` is only the legacy close wrapper.
- FINREG readers already test generation liveness with acquire loads of
  `t->metatable`; disabling now matches that publication protocol.

Verification:

- tools/ci/lua_test.sh m7_ffi_finreg
- tools/ci/lua_test.sh m8_weak

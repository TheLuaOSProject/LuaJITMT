# FFI GC Trace Expectation

- `ffi.gc(cd, fn)` and `ffi.gc(cd, nil)` are currently interpreter fallback
  paths: `recff_ffi_gc()` raises `LJ_TRERR_NYICONV` rather than recording the
  public `ffi.gc` call.
- `tests/t-ffi-gc-trace.lua` still expected three recorder traces, which was
  stale after the recorder was made NYI again. The remaining traced coverage is
  the metatype `__gc` allocation path, which still records through
  `crec_alloc()`/`crec_finalizer()` and emits `lj_cdata_setfin`.
- The test now asserts at least one metatype finalizer trace and keeps direct
  `ffi.gc` / clear coverage as semantic checks.
- The M7 suite still dump-checks `lj_cdata_setfin`, so loss of metatype
  finalizer recording remains visible.
- Verification:
  - `tools/ci/lua_test.sh m7_ffi_finreg`

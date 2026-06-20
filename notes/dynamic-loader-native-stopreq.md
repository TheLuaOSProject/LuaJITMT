2026-06-20

- Pauli identified dynamic library loading as a remaining native-state gap.
- Wrapped Linux `package.loadlib()` dynamic-loader operations around
  `dlopen`, `dlsym`, bytecode-symbol `dlsym`, and L-aware `dlclose`.
- Wrapped Linux FFI CLibrary `dlopen`, `dlsym`, and L-aware `dlclose`.
- Kept `lj_udata_free()`'s no-`lua_State` fallback raw instead of inventing a
  fake owner. The `ffi_clib___gc` path now passes `L`, so normal finalizer
  unloads can check STOPREQ after nulling the handle.
- Added a tiny sleeping-constructor shared object and wired it into
  `m3_safepoint_handshake` to behavior-test `package.loadlib()` and
  `ffi.load()` while blocked in `dlopen`.
- Validation:
  - `tools/ci/lua_test.sh m3_safepoint_handshake`
  - `tools/ci/lua_test.sh m7_ffi_clib_cache`
  - serial normal `package.loadlib()` / `ffi.load()` smoke against the test .so

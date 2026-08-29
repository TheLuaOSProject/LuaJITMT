# Recorded C Library Namespace Helper Loads

`recff_clib_index()` now snapshots cached `ffi.C` symbol metadata with
`ctype_info_acq()` / `ctype_size_acq()` before recording namespace constants or
extern symbol conversions.

The helper-backed snapshots cover constant-value detection, large unsigned
constant widening to a numeric IR constant, the raw child ctype used for extern
symbol conversion, and extern ctype id selection before the recorder converts a
cached cdata symbol into the requested value form.

The post-lookup helpers deliberately avoid `ctype_child()` and `ctype_raw()` in
the recorder. `crec_ctype_rawchild()` snapshots the const-value child before
checking `CTF_UNSIGNED`, and `crec_ctype_rawid()` snapshots the extern target
type before `crec_tv_ct()` / `crec_ct_tv()` emit the load or store conversion.

Validation:

- `make -C src -j2`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_clib_cache`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `git diff --check`

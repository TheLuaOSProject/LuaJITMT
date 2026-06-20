Shared-library test helpers
===========================

Context
-------

Several Lua suite cases built temporary shared libraries by hand with repeated
compiler command assembly. The FFI ld-script case also wrote a temporary GNU ld
script inline.

Fix
---

* `suite_utils.write_file()` provides a shared file-writing primitive.
* `suite_build.build_shared_library()` centralizes temporary `.so` compilation.
* `suite_build.write_ld_script()` centralizes the GNU ld-script fixture text.
* M3 loadlib STOPREQ and M7 FFI clib ld-script cases now use the shared helpers.

Validation
----------

* `tools/ci/lua_test.sh m3_safepoint_handshake m7_ffi_clib_ldscript`

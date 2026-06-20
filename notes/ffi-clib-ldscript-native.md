2026-06-20

- Wrapped FFI CLibrary GNU ld-script resolution reads in native state:
  `fopen`, `fgets`, and `fclose` now leave through the safepoint machinery.
- The path matters on Linux because `ffi.load()` may parse linker scripts after
  a failed `dlopen`; those file operations can block on unusual or remote
  filesystems and should not leave the mutator invisible to handshakes.
- Added `m7_ffi_clib_ldscript`, which builds a tiny shared object, writes a GNU
  ld script pointing at it, loads the script with `ffi.load()`, and calls a real
  symbol through the resolved CLibrary.
- Validation:
  - `tools/ci/lua_test.sh m7_ffi_clib_ldscript`
  - `tools/ci/lua_test.sh m7_ffi_clib_cache`
  - `LUA_PATH='tests/lib/?.lua;tests/suites/?.lua;;' ./src/luajit -e "require('m7_ffi')"`
- A direct stock `sysdep/ffi_lib_c.lua` run currently crashes in the nested
  `ffi.C.luaL_newstate()` / `lua_close()` section. This reproduces in a clean
  detached worktree at `e288d434`, so it predates this slice and should be
  handled separately.

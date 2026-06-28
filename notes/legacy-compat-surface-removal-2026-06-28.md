# Legacy and compatibility surface removal

This cleanup removes stale compatibility entry points that conflicted with the
current lockless policy:

- Deleted unsupported Windows and console batch build scripts.
- Rewrote `doc/install.html` for the only supported target: x86-64 Linux with
  GC64.
- Updated public docs and Makefiles so they no longer advertise MSVC/MinGW,
  console, non-x64, GC32, or Lua 5.2 compatibility builds.
- Made `-DLUAJIT_ENABLE_LUA52COMPAT` fail explicitly in `lj_arch.h`.
- Removed stale v2 bytecode compatibility plan/test material and replaced it
  with current-only dump validation wording.
- Removed redundant `string.gmatch` C-closure and local-cell x64 source guards;
  behavior fixtures, bytecode checks, and generated JIT dump checks cover the
  observable semantics.

Kept intentionally:

- GC "legacy" bridge names and weak fallback counters while they describe real
  safety bridge behavior.
- FFI C type compatibility helpers; these are language/FFI semantics, not
  old public C API aliases.

Verification:

- `tools/ci/lua_test.sh m5_bcdump_current m5_upvalue_publish_gc`
- `tools/ci/lua_test.sh m5_cell_ops m6_jit_cell_ops`
- `make -C src XCFLAGS=-DLUAJIT_ENABLE_LUA52COMPAT` fails with the lockless
  compatibility-mode error.
- `make -C src -j`

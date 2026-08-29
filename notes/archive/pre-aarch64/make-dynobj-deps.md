# Makefile dynamic-object dependencies

- Split `_dyn.o` builds out of the ordinary `.o` pattern rules so the shared
  library target can depend on the exact dynamic objects it links.
- Each `_dyn.o` target depends on its matching static object.  The static object
  dependencies in `Makefile.dep` already carry generated-header ordering, so a
  clean parallel build cannot start dynamic compilation before headers such as
  `lj_bcdef.h` and `lj_ffdef.h` exist.
- This also fixes partially cleaned trees where static objects remain but their
  dynamic sidecars are missing: `libluajit.so` now regenerates the sidecars
  instead of linking stale or absent files.

Validation:

- `make -C src clean`
- `make -C src -j$(nproc)`
- `tools/ci/lua_test.sh m9_m10_gc`
- `src/luajit -e "require'ffi'; print(2^31)"`
- `git diff --check`

# Harness build-profile restoration

Some tests intentionally switch `XCFLAGS`, including `-DLUAJIT_DISABLE_JIT`.
That profile makes the host `buildvm` fold generator compile its no-JIT stub,
so `lj_folddef.h` is legitimately empty for the no-JIT build.

If such a test leaves the source tree in that profile, a later plain `make`
does not know the command-line flags changed and can reuse the no-JIT generated
header/host objects for a JIT-enabled build. The visible failure is a missing
`fold_hashkey`/`fold_hash`/`fold_func` compile error in `lj_opt_fold.c`.

The harness now has `build.with_default_build_restore()` for profile-switching
tests. It runs the test body, then performs a clean default build even on test
failure, preserving the original error if the test failed.

Applied to:

- `m0_build_profile_switch`
- `m0_matrix`
- `m0_lua52_compat`
- `m3_gc2_paranoia`

Verification target:

- `tools/ci/lua_test.sh m3_gc2_paranoia`
- `tools/ci/lua_test.sh m0_build_profile_switch`
- `tools/ci/lua_test.sh m0_matrix`
- `make -C src -j$(nproc)`

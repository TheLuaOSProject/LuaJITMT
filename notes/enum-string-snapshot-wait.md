# Enum string snapshot wait slice

This slice removes parser-lock fallback lookups from enum string constant
conversion while preserving normal hit and miss semantics.

Changed:

- `lj_ctype_enumconst_snapshot()` now uses the common snapshot completion check
  before returning stable misses.
- `lj_ctype_enumconst_wait()` retries the enum snapshot after parser-token
  waits and takes a `CTypeID`, not a borrowed `CType *`, so it refetches the
  enum root after any native wait.
- `lj_carith.c` and `lj_cconv.c` now use `lj_ctype_enumconst_wait()` instead
  of taking the parser token and calling `lj_ctype_getfield()`.
- `t-ffi-enum-snapshot.c` now holds the parser token across enum hit and miss
  conversions and checks the parser sequence advances only from the helper
  release.
- `m7_ffi_typeinfo_snapshot.sh` now guards against restoring enum parser-lock
  fallback text, early raw enum snapshot misses, or a pointer-taking enum wait
  helper.

Why this matters:

- Enum string conversion is another public interpreter FFI reader that can keep
  normal Lua semantics with snapshot wait/retry.
- Misses are as important as hits: a transient abandoned enum constant must not
  become a user-visible false miss or bad-conversion error.
- Refetching by `CTypeID` avoids carrying old ctype-table pointers across the
  native futex wait used while another parser owns the mutation token.

Verification:

- `make -C src -j`
- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `tools/ci/lua_test.sh m7_ffi_carith_l m7_ffi_ctype_tab_retire m7_ffi_cparse_rollback`
- `tools/ci/lua_test.sh m7_ffi`
- `git diff --check`

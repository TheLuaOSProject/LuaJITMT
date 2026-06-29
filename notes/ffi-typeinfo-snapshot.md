# FFI ctype metadata snapshot slice

- Converted CTState.parse_token from a binary parser lock flag into an
  even/odd sequence. Odd means a cparser mutation/rollback transaction is
  active; unlock publishes the next even value and wakes waiters.
- Added lj_ctype_snapshot() for stable CType record reads. It copies the
  record only when the parser sequence is even and unchanged across the copy;
  active or overlapping parser work asks the caller to retry under the parser
  lock.
- `ffi.typeinfo(id)` is an internal and unsupported LuaJIT 2.1 FFI entrypoint.
  It stays in the public `ffi` table for stock compatibility, but reads through
  lockless CType snapshots and returns `nil` while parser-created IDs are in an
  active mutation/rollback window.
- Added tests/t-ffi-typeinfo-snapshot.c and m7_ffi_typeinfo_snapshot. The C
  fixture asserts `ffi.typeinfo()` and stable public readers such as
  `ffi.typeof()`, `ffi.sizeof()`, and `ffi.alignof()` do not advance parse_token
  while `ffi.cdef()` still does, and the suite also runs the rollback-reader
  race against stock-visible FFI operations.

Verification:

- tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot
- tools/ci/lua_test.sh m7_ffi_cdef_token
- tools/ci/lua_test.sh m7_ffi_cparse_rollback
- tools/ci/lua_test.sh m7_ffi_ctype_name_claim

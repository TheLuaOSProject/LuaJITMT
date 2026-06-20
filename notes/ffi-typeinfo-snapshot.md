FFI typeinfo snapshot slice

- Converted CTState.parse_token from a binary parser lock flag into an
  even/odd sequence. Odd means a cparser mutation/rollback transaction is
  active; unlock publishes the next even value and wakes waiters.
- Added lj_ctype_snapshot() for stable CType record reads. It copies the
  record only when the parser sequence is even and unchanged across the copy;
  active or overlapping parser work asks the caller to retry under the parser
  lock.
- Routed ffi.typeinfo(id) through the snapshot helper first. Stable published
  ctypes no longer acquire the cparser mutation sequence; active parser windows
  retain the old locked behavior so rollback remains hidden.
- Added tests/t-ffi-typeinfo-snapshot.c and m7_ffi_typeinfo_snapshot. The C
  fixture asserts stable ffi.typeinfo() calls do not advance parse_token while
  ffi.cdef() still does, and the suite also runs the existing rollback-reader
  race.

Verification:

- tools/ci/m7_ffi_typeinfo_snapshot.sh
- tools/ci/lua_test.sh m7_ffi_cdef_token
- tools/ci/lua_test.sh m7_ffi_cparse_rollback
- tools/ci/lua_test.sh m7_ffi_ctype_name_claim

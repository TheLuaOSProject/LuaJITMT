# FFI layout wait helpers

Stable CType-backed `ffi.new`, `ffi.cast`, `ffi.sizeof`, `ffi.alignof`, and
`ffi.offsetof` no longer fall back to acquiring the ctype parser token when a
sequence-checked snapshot races active parser mutation. They now retry through
local wait helpers that park in native state via `lj_ctype_parse_wait()` and
re-run the snapshot after the parser sequence is published.

Follow-up: stable `ffi.alignof(ct)` misses for abandoned/invalid CType IDs now
terminate from the snapshot path and return `nil`, matching `ffi.sizeof(ct)`,
instead of falling through into the parser-lock layout fallback.

String declarations still use the serialized parser path because they actually
mutate the C type graph. The new behavior is guarded by
`tests/t-ffi-layout-snapshot.c`, which holds the parse token from a helper
thread and verifies each stable layout/type operation waits natively without
advancing the parser sequence through a lock/unlock pair.

Verification:

- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `tools/ci/lua_test.sh m7_ffi_cparse_rollback`

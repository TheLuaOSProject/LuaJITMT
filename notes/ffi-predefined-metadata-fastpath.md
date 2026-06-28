# FFI predefined metadata/layout reads

Predefined C type records are immutable after VM initialization. FFI metadata
and layout queries for those IDs no longer wait for an unrelated active
`ffi.cdef()` parser token.

This affects the interpreter-side readers used by `ffi.cast(ct, value)`,
`ffi.new(ct, ...)`, `ffi.sizeof(ct)`, and `ffi.alignof(ct)` when the ctype ID
and any child/raw/enum walk stay inside the predefined range. If a walk leaves
that range, or the predefined table does not have the expected initialized
slots, the code falls back to the existing sequence-checked snapshot and
wait/retry path.

User-defined records, string declarations, `ffi.offsetof()` field walks, VLA
types created by parsing, and parser rollback visibility keep the existing
guarded snapshot behavior.

Validation target:
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`

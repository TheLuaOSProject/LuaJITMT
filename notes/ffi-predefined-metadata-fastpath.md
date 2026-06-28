# FFI predefined metadata/layout reads

Predefined C type records are immutable after VM initialization. FFI metadata
and layout queries for those IDs no longer wait for an unrelated active
`ffi.cdef()` parser token.

This affects the interpreter-side readers used by `ffi.typeinfo(id)`,
`ffi.cast(ct, value)`, `ffi.new(ct, ...)`, `ffi.sizeof(ct)`, and
`ffi.alignof(ct)` when the ctype ID and any child/raw/enum walk stay inside the
predefined range. If a walk leaves that range, or the predefined table does not
have the expected initialized slots, the code falls back to the existing
sequence-checked snapshot and wait/retry path.

Recorder-side raw-reference and child metadata reads also check the immutable
predefined range before raising `LJ_TRERR_CTBUSY`. This lets traced
`tonumber(int64_t)` and 64-bit `bit.*` operations on predefined scalar cdata
record under an unrelated active parser token. Parser-created enum, struct,
array, and field metadata still use sequence-checked snapshots and abort
recording instead of waiting inside the recorder.

User-defined records, string declarations, `ffi.offsetof()` field walks, VLA
types created by parsing, and parser rollback visibility keep the existing
guarded snapshot behavior.

Validation target:
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`

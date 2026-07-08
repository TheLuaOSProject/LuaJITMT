# FFI predefined metadata nonblocking snapshot

`ffi.typeinfo()` is an internal and unsupported LuaJIT 2.1 FFI helper, and it
must stay in the Lua `ffi` table for stock compatibility. It shares the same
nonblocking predefined CType metadata rules as stock-visible operations such as
cdata arithmetic, layout queries, and recorder fallbacks.

This keeps mutable `ffi.cdef()` serialized while removing parser-lock fallbacks
from non-mutating readers. Predefined CType IDs are installed during VM
initialization and are immutable, so their metadata can be read even while an
unrelated `ffi.cdef()` transaction is active.

The shared `lj_ctype_info_predefined()` helper now owns the immutable
predefined metadata walk. Interpreted wait paths and cdata arithmetic reuse
that helper before consulting the parser sequence. This keeps fallback readers
from waiting or aborting recording on unrelated parser work just to inspect an
immutable scalar CType.

`ffi.sizeof()` and pointer-shaped `ffi.alignof()` now reuse the direct string
layout readers in the interpreter, too. Primitive pointer/array layout queries
can return size/alignment without constructing temporary CType records, while
unsupported declarations still fall back to the normal parser path and preserve
stock errors and VLA handling.

Coverage:

- `tests/t-ffi-typeinfo-snapshot.c` manually holds `CTState.parse_token` odd,
  verifies predefined `int` metadata stays readable through `ffi.typeinfo()`
  and stock operations, then verifies parser-created metadata reads wait in
  native time, succeed after release, and do not advance the parser sequence.
- `tests/t-ffi-metatv-snapshot.c` holds the parser token while a predefined
  `int` cdata arithmetic miss flows through type-info and ctype-metamethod
  fallback without waiting.
- `tests/t-ffi-recorder-metatv-busy.c` holds the parser token during trace
  recording and verifies the predefined arithmetic miss does not report a
  parser-busy abort.
- `tests/t-ffi-cparse-rollback-reader.lua` verifies failed `ffi.cdef()`
  rollback remains hidden from FFI readers such as `ffi.typeinfo()`,
  `ffi.sizeof()`, `ffi.new()`, cdata field/numeric access, pointer arithmetic,
  enum casts, and `ffi.C`; the `ffi.typeinfo()` path now rejects transient
  active-parser `nil` results for an existing stable ctype ID.
- `tests/t-ffi-layout-snapshot.c` verifies direct primitive pointer/array
  `ffi.sizeof()` and pointer `ffi.alignof()` strings do not advance either the
  parser sequence or ctype top.

Verification:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/lua_test.sh m7_ffi_snap_restore_l`
- `tools/ci/lua_test.sh m7_ffi_cparse_rollback`
- `tools/ci/lua_test.sh m7_ffi_ccall_native`

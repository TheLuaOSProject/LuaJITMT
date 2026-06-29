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

Coverage:

- `tests/t-ffi-typeinfo-snapshot.c` manually holds `CTState.parse_token` odd,
  verifies predefined `int` metadata stays readable through `ffi.typeinfo()`
  and stock operations, then verifies parser-created metadata reads return
  `nil` while the parser is active and succeed after release without advancing
  the parser sequence.
- `tests/t-ffi-metatv-snapshot.c` holds the parser token while a predefined
  `int` cdata arithmetic miss flows through type-info and ctype-metamethod
  fallback without waiting.
- `tests/t-ffi-recorder-metatv-busy.c` holds the parser token during trace
  recording and verifies the predefined arithmetic miss does not report a
  parser-busy abort.
- `tests/t-ffi-cparse-rollback-reader.lua` verifies failed `ffi.cdef()`
  rollback remains hidden from FFI readers such as `ffi.typeinfo()`,
  `ffi.sizeof()`, `ffi.new()`, cdata field/numeric access, pointer arithmetic,
  enum casts, and `ffi.C`.

Verification:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/lua_test.sh m7_ffi_cparse_rollback`
- `tools/ci/lua_test.sh m7_ffi_ccall_native`

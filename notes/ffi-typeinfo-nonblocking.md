# FFI predefined metadata nonblocking snapshot

The old public `ffi.typeinfo()` helper was internal and nonstock, so it has
been removed from the Lua `ffi` table. The implementation still needs
nonblocking predefined CType metadata reads for stock-visible operations such
as cdata arithmetic, layout queries, and recorder fallbacks.

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
  verifies predefined `int` metadata stays readable through stock operations,
  then verifies parser-created metadata reads succeed after release without
  advancing the parser sequence.
- `tests/t-ffi-metatv-snapshot.c` holds the parser token while a predefined
  `int` cdata arithmetic miss flows through type-info and ctype-metamethod
  fallback without waiting.
- `tests/t-ffi-recorder-metatv-busy.c` holds the parser token during trace
  recording and verifies the predefined arithmetic miss does not report a
  parser-busy abort.
- `tests/t-ffi-cparse-rollback-reader.lua` verifies failed `ffi.cdef()`
  rollback remains hidden from stock-visible FFI readers such as `ffi.sizeof()`,
  `ffi.new()`, cdata field/numeric access, pointer arithmetic, enum casts, and
  `ffi.C`.

Verification:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/lua_test.sh m7_ffi_cparse_rollback`
- `tools/ci/lua_test.sh m7_ffi_ccall_native`

# FFI ctype metamethod snapshots

Interpreter ctype metamethod lookup now has a sequence-checked wait path:
`lj_ctype_metatv_wait()`. It snapshots the ctype record chain used for
attribute/reference stripping and the function-pointer special case before
reading the ctype metatable side map or the shared function-pointer metatable.

The interpreter FFI paths for cdata `__index`, `__newindex`, `__call`,
`__tostring`, `__pairs`, `__ipairs`, constructor `__gc`, and cdata arithmetic
metamethods use the wait helper. Pointer-wrapper stripping in `__call`,
`pairs`, and arithmetic dispatch is also snapshot-backed.

Immutable predefined CType IDs that cannot legally have ctype metatables now
return "no metamethod" without consulting the parser sequence. The shared
`lj_ctype_predefined_nometa()` predicate covers scalar, enum, void, array, and
non-function-pointer predefined IDs. Predefined complex/vector types and all
parser-created types keep the snapshot/wait path, because `ffi.metatype()` can
attach tables to complex/vector and struct-like records.

Recorder FFI ctype metamethod lookups use the same predefined no-metatable
predicate before `lj_ctype_metatv_snapshot()`. For parser-created and
metatable-capable types, a busy parser token still aborts recording with
`LJ_TRERR_CTBUSY` instead of waiting from the recorder; normal execution then
falls back to the interpreter path.

While covering `__newindex`, pointer auto-deref now preserves qualifiers from
the pointed-to struct. A write through `const struct *` therefore still fails
before consulting table-backed `__newindex`, matching LuaJIT's stock FFI
semantics.

Coverage:
- `tests/t-ffi-metatv-snapshot.c` checks stable `__call`, `__add`, `__pairs`,
  and constructor `__gc` lookup do not advance `CTState.parse_token`.
- The same fixture holds the parser token and verifies predefined `int`
  construction plus bad-member lookup avoid waiting, while struct `__call`,
  `__add`, and `__pairs` wait from a native region before dispatching the
  metamethod.
- `tests/t-ffi-recorder-metatv-busy.c` holds the parser token during trace
  recording and verifies predefined `int` construction does not hit a
  parser-busy abort, while ctype `__call`, `__add`, table-backed `__index`,
  and `ffi.new()`/`__gc` lookup on parser-created metatypes still abort
  recording rather than waiting.
- The fixtures are wired into `m7_ffi_metatype`; this is behavior coverage,
  not a source-search guard.

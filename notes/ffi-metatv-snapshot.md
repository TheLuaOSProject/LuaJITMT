# FFI ctype metamethod snapshots

Interpreter ctype metamethod lookup now has a sequence-checked wait path:
`lj_ctype_metatv_wait()`. It snapshots the ctype record chain used for
attribute/reference stripping and the function-pointer special case before
reading the ctype metatable side map or the shared function-pointer metatable.

The interpreter FFI paths for cdata `__index`, `__newindex`, `__call`,
`__tostring`, `__pairs`, `__ipairs`, constructor `__gc`, and cdata arithmetic
metamethods use the wait helper. Pointer-wrapper stripping in `__call`,
`pairs`, and arithmetic dispatch is also snapshot-backed.

While covering `__newindex`, pointer auto-deref now preserves qualifiers from
the pointed-to struct. A write through `const struct *` therefore still fails
before consulting table-backed `__newindex`, matching LuaJIT's stock FFI
semantics.

Recorder-side ctype metamethod lookups still use the existing non-waiting
helper and remain a separate JIT cleanup target.

Coverage:
- `tests/t-ffi-metatv-snapshot.c` checks stable `__call`, `__add`, `__pairs`,
  and constructor `__gc` lookup do not advance `CTState.parse_token`.
- The same fixture holds the parser token and verifies `__call`, `__add`, and
  `__pairs` wait from a native region before dispatching the metamethod.
- The fixture is wired into `m7_ffi_metatype`; this is behavior coverage, not a
  source-search guard.

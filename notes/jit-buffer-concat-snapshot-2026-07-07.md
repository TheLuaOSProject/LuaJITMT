JIT buffer concat snapshots

- `BC_CAT` recording now treats `string.buffer` userdata as concat-compatible
  alongside strings and numbers.
- Buffer operands are guarded on `UDTYPE_BUFFER`, converted to their
  `SBufExt` payload pointer, and snapshotted through
  `lj_bufx_tostr_forjit()` before entering the normal `BUFHDR`/`BUFPUT`/
  `BUFSTR` concat chain.
- This keeps user buffer concat on the existing traced string-construction
  path instead of falling through to `__concat` metamethod recording, where
  buffers intentionally have no metamethod table entry.

Validation:

- direct `buffer` concat JIT trace smoke
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m6_jit_buffer_method_shared_nyi`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m6_jit_mcode_publish`
- `src/luajit tests/stock/test/lang/concat.lua`

`tools/ci/lua_test.sh m6_jit` reached `m6_jit_mcode_publish` and then timed out
once in `tests/t-jit-mcode-fresh.lua`; that file and the named
`m6_jit_mcode_publish` gate both passed immediately when rerun in isolation.

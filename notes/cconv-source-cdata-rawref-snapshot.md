# CConv source cdata raw-ref snapshot

`lj_cconv_multi_init_l()` now resolves a cdata initializer's raw source type
with `lj_ctype_rawref_snapshot()` and waits through `lj_ctype_parse_wait()` if
the ctype parser owns the mutation token.

The destination aggregate snapshot already waited before aggregate
initialization, but another thread can start `ffi.cdef()` before the
source-cdata identity check. The old `ctype_rawrefid()` walk followed live ctype
records directly in that window, so it could observe parser-owned rollback
state instead of the published sequence boundary.

Predefined ctype IDs still use the no-wait predefined raw-ref fast path. Parser
created IDs use the sequence-checked snapshot and only park when the ctype
mutation token is actively held.

Coverage:

- `tests/t-ffi-cconv-init-snapshot.c` now holds the parser token while calling
  `lj_cconv_multi_init_l()` on an identical source/destination aggregate cdata
  initializer.
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`

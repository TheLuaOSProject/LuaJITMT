# CData shallow container snapshot rationale

`lj_cdata_index_l()`, `lj_cdata_get_l()`, and `lj_cdata_set_l()` enter through
`cdata_ctype_snapshot_wait()`, which first takes a shallow copy of the current
cdata object's container record. That shallow copy remains intentionally
allowed for already-published parser-created container IDs.

Cdata objects cannot point at an uncommitted parser rollback record: their
`ctypeid` is stored after the type has been published. Existing ctype records
are immutable after publication, and old `CTypeTab` arrays are retained through
the ctype-table SMR path. Keeping this shallow container snapshot preserves the
documented no-wait path for already-published `int *`-style records while
unrelated `ffi.cdef()` work is active.

Rollback-sensitive work is still routed through sequence-checked helpers:
field lookup, constructor constants, pointer auto-deref to parser-created
structs, enum/string conversion, and element-size lookup for parser-created
children all use ID-rooted snapshot/wait helpers.

This slice makes two narrower fixes from the audit:

- `lj_ctype_getname_snapshot()` now validates the parser sequence before
  returning "not found" for an abandoned redirected sibling.
- `lj_cdata_new_l()` debug allocation checks now use ctype helper loads.

Coverage:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/lua_test.sh m7_ffi_cdata_set_l`
- `tools/ci/lua_test.sh m7_ffi_cdata_get_l`
- `tools/ci/lua_test.sh m7_ffi_cparse_rollback`

# `ffi.istype()` predefined ctype comparisons

Predefined C types are initialized before user parsing and their records are
immutable. `ffi.istype(ct, value)` can compare pairs whose effective ctype IDs
both refer to those predefined records without waiting for an unrelated active
`ffi.cdef()` parser mutation.

The normal sequence-checked snapshot path remains in place for user-defined
types, string declarations, attributes allocated by parsing, and any comparison
that walks outside the predefined range. Those cases still wait/retry so parser
rollback and in-progress type publication stay hidden from readers.

`tests/t-ffi-istype-snapshot.c` now holds the parser token while checking
predefined `int`/`uint8_t` ctype-object and cdata comparisons. The same fixture
still verifies that a user-defined struct comparison parks in native state and
retries after the parser token is released.

Validation target:
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`

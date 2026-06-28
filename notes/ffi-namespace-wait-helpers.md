# FFI namespace wait helpers

Stable `ffi.C` namespace lookups no longer fall back to acquiring the ctype
parser token when the sequence-checked name snapshot races active parser
mutation. `lj_ctype_getname_wait()` now owns the retry loop: it waits in native
state with `lj_ctype_parse_wait()` and re-runs the name snapshot after the
parser sequence is published.

`ffi.cdef` and string-backed type parsing remain serialized. This only changes
read-only namespace resolution for already-published CType names, including
constants, ordinary C symbols, and redirected `asm("...")` symbols.

Verification:

- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `tools/ci/m7_ffi_clib_cache.sh`
- `tools/ci/lua_test.sh m7_ffi_cparse_rollback`

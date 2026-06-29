# FFI bit64 cdata snapshot

`lj_carith_check64()` no longer walks cdata ctype metadata directly with raw
`ctype_get()` / `ctype_rawid()` / payload loads while another parser may own
`CTState.parse_token`.

The bit-library cdata conversion path now snapshots or waits through the local
`carith_ctype_info_read()` helper, carries only stable source IDs and scalar
metadata across parser waits, and refetches the final source `CType *` by ID
before calling `lj_cconv_ct_ct_l()`.

Coverage lives in `tests/t-ffi-carith-check64-snapshot.c`, wired into
`m7_ffi_carith_l`. The fixture holds the parser token while exercising
`bit.band`, `bit.bxor`, and `bit.tohex` on parser-created enum cdata, requiring
native parser wait behavior, while predefined `uint64_t` cdata continues to
avoid waiting.

Validation:

- `tools/ci/lua_test.sh m7_ffi_carith_l`

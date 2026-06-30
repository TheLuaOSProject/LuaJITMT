serialize cdata helper loads

- Routed `serialize_put()` through `lj_ctype_info_wait()` before selecting the
  serializer tag for signed 64-bit, unsigned 64-bit, and complex cdata values.
  The tag decision still uses the raw `CType` snapshot so enum/reference cdata
  keep the old unsupported-cdata behavior.
- Extended `tools/ci/m7_ffi_cdata_get_l.sh` to reject raw `CType.info` and
  `CType.size` reads in the cdata serialization branch.
- Extended `tests/t-ffi-cdata-get-l.lua` to round-trip the supported
  serialized cdata forms through `string.buffer`, and
  `tests/t-ffi-tonumber-snapshot.c` to hold the parser token while
  `buffer.encode()` serializes a parser-owned qualified int64 cdata value.

Verification:

- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m7_ffi_cdata_get_l`
- `LJ_TEST_DISABLE_BUILD_CACHE=1 tools/ci/lua_test.sh m5_buffer_publish`

serialize cdata helper loads

- Routed `serialize_put()` through `ctype_info_acq()` and `ctype_size_acq()`
  before selecting the serializer tag for signed 64-bit, unsigned 64-bit, and
  complex cdata values.
- Extended `tools/ci/m7_ffi_cdata_get_l.sh` to reject raw `CType.info` and
  `CType.size` reads in the cdata serialization branch.
- Extended `tests/t-ffi-cdata-get-l.lua` to round-trip the supported serialized
  cdata forms through `string.buffer`.

Verification:

- `tools/ci/m7_ffi_cdata_get_l.sh`

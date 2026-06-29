# FFI aggregate conversion/init snapshots

The interpreter aggregate conversion helpers in `lj_cconv.c` now snapshot or
wait on ctype metadata before walking array children, struct field chains, and
subtype field nodes.

This covers table-to-array, table-to-struct, multi-value array initialization,
and multi-value struct initialization. The helpers use exact ctype snapshots
for field-chain nodes so `CTA_SUBTYPE` attributes are not skipped by accident,
use raw child snapshots only where a destination field or array element is
needed, and refetch live `CType *` pointers by stable IDs after recursive
`lj_cconv_ct_tv_l()` calls that may wait on enum constants or intern pointer
ctypes.

Predefined scalar initialization keeps a no-wait fast path for immutable
predefined IDs, avoiding a parser-token wait for normal `ffi.new(int_ct, v)`
style conversions.

Coverage lives in `tests/t-ffi-cconv-init-snapshot.c`, wired into
`m7_ffi_typeinfo_snapshot`. The fixture holds the parser token across
parser-created enum array/struct initializers from both table and multi-value
forms, and separately verifies predefined scalar initialization does not wait.

Still separate follow-up work: recorder aggregate initialization paths in
`lj_crecord.c` need CTBUSY snapshot/abort discipline rather than interpreter
waiting, and broader TValue-to-C conversion helpers still have non-aggregate
raw child readers such as string-to-array conversion.

Validation:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`

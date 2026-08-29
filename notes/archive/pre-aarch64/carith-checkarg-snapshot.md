# FFI arithmetic argument snapshots

`carith_checkarg()` now uses the local ctype info snapshot/wait helper when it
normalizes cdata operands for general FFI arithmetic and comparisons.

The old path walked `ctype_rawid()` and cached live `CType *` entries while the
ctype parser could own `CTState.parse_token`. The new cdata helper copies raw
ctype metadata through `carith_ctype_info_read()`, classifies pointer,
reference, function, and enum operands from the copied record, and refetches
final `CType *` pointers by stable IDs. If a parser wait or pointer-type intern
can grow the ctype table, previously normalized operands are refreshed by ID.

There is one no-wait compatibility fast path: a shallow pointer/reference ctype
record whose child is predefined may be copied directly into the local
`CDArith` snapshot while another parser holds the token. This preserves
existing `int *` pointer arithmetic behavior where the later element-size read
is known to avoid waiting for predefined children. Parser-created enum/struct
paths still use the full snapshot/wait reader.

The enum-string branch also reads the other operand's raw enum ctype through
the same snapshot/wait path before calling `lj_ctype_enumconst_wait()`, so
`"ENUM_CONST" == enum_cdata` no longer samples raw ctype table entries while
the parser token is busy.

Coverage lives in `tests/t-ffi-carith-arg-snapshot.c`, wired into
`m7_ffi_carith_l`. The fixture runs parser-token release coverage across
parser-created enum arithmetic, enum-string comparisons, and pointer
arithmetic, while predefined `int64_t` and `int *` arithmetic still avoid
waiting.

Validation:

- `tools/ci/lua_test.sh m7_ffi_carith_l`

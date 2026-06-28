# Predefined string ctype fast path

Exact parameter-free FFI type strings that name immutable predefined C types
now resolve directly to their stable `CTID_*` values in `ffi_checkctype()`.
This covers common scalar spellings such as `int`, `double`, `size_t`, the
fixed-width integer typedefs, and the pointer spellings that already have
predefined CTIDs: `void *`, `const void *`, `const char *`, and `uint8_t *`.
Leading and trailing C whitespace is ignored before matching these exact
spellings.

The reason is concurrency, not just speed: these names are installed during
`lj_ctype_init()` before the `CTState` is shared, and their payload records are
immutable. Resolving them does not need the cparser mutation token and does not
participate in parser rollback.

General declarations still use the parser path. That includes pointer forms
without a predefined CTID, arrays, qualifiers that need attribute records,
structs/unions/enums, function types, declarations with `$` parameters,
variable-length forms, and strings whose internal spacing or token sequence
does not exactly match the predefined table. Those can allocate or intern ctype
records, observe rollback-sensitive names, or need normal parser diagnostics.

Validation target:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`

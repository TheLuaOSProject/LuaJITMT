# Predefined string ctype fast path

Exact parameter-free FFI type strings that name immutable predefined C types
now resolve directly to their stable `CTID_*` values in `ffi_checkctype()`.
This covers common scalar spellings such as `int`, `double`, `size_t`, the
fixed-width integer typedefs, and the pointer spellings that already have
predefined CTIDs: `void *`, `const void *`, `const char *`, and `uint8_t *`.
Leading and trailing C whitespace is ignored before matching these exact
spellings.

Exact already-published typedef identifiers also bypass the parser token on the
stable path. `ffi.typeof("my_typedef")`, `ffi.sizeof("my_typedef")`, and
similar parameter-free APIs first resolve the name with the ctype namespace
snapshot/wait helpers and return the typedef target ID. If another parser owns
the token, the lookup waits in native time and retries; if the string is not a
single typedef identifier, the existing parser path handles the declaration and
its diagnostics.

The reason is concurrency, not just speed. Predefined names are installed
during `lj_ctype_init()` before the `CTState` is shared, and their payload
records are immutable. Published typedef names are not immutable in the same
way, so their fast path uses the ctype namespace snapshot/wait helpers and
falls back to the parser if the string is not a direct typedef lookup.

General declarations still use the parser path. That includes pointer forms
without a predefined CTID, arrays, qualifiers that need attribute records,
structs/unions/enums, function types, declarations with `$` parameters,
variable-length forms, and strings whose internal spacing or token sequence
does not exactly match the predefined table or a single typedef identifier.
Those can allocate or intern ctype records, observe rollback-sensitive names,
or need normal parser diagnostics.

Validation target:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`

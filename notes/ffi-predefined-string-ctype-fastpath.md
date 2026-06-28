# Predefined string ctype fast path

Exact parameter-free FFI type strings that name immutable predefined C types
now resolve directly to their stable `CTID_*` values in `ffi_checkctype()`.
This covers common scalar spellings such as `int`, `double`, `size_t`, the
fixed-width integer typedefs, and the pointer spellings that already have
predefined CTIDs: `void *`, `const void *`, `const char *`, and `uint8_t *`.
Leading and trailing C whitespace is ignored before matching these exact
spellings. The immutable predefined const bases `const void`, `void const`,
`const char`, and `char const` are also direct matches, so pointer chains over
those bases do not need a parser round trip.

Exact already-published typedef identifiers and simple tag names also bypass
the parser token on the stable path. `ffi.typeof("my_typedef")`,
`ffi.sizeof("struct my_tag")`, `ffi.typeof("union my_tag")`, and similar
parameter-free APIs first resolve the name with the ctype namespace
snapshot/wait helpers and return the published ID. If another parser owns the
token, the lookup waits in native time and retries; if the string is not a
single typedef identifier or `struct`/`union`/`enum` tag lookup, the existing
parser path handles the declaration and its diagnostics.

Trailing pointer declarator chains over those direct bases also use this path:
`ffi.typeof("my_typedef **")`, `ffi.typeof("struct my_tag **")`, and
predefined scalar bases such as `ffi.typeof("int **")` resolve the base
without the parser token and then intern each pointer ctype through the
lock-free ctype intern table. The base-name wait still happens in native time
when a parser is active, so the lookup does not read rollback-sensitive names
while the parser token is held.

The reason is concurrency, not just speed. Predefined names are installed
during `lj_ctype_init()` before the `CTState` is shared, and their payload
records are immutable. Published typedef names are not immutable in the same
way, so their fast path uses the ctype namespace snapshot/wait helpers and
falls back to the parser if the string is not a direct typedef or tag lookup.

General declarations still use the parser path. That includes arrays,
qualifiers that need attribute records, structs/unions/enums, function types,
qualified pointer chains that are not exact predefined spellings, declarations
with `$` parameters, variable-length forms, and strings whose internal spacing
or token sequence does not exactly match the predefined table, a single typedef
identifier, a simple tag lookup, or a trailing pointer chain over one of those
bases. Those can allocate or intern ctype records, observe rollback-sensitive
names, or need normal parser diagnostics.

Validation target:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`

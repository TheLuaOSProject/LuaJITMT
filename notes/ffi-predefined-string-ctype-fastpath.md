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

Predefined complex primitives use the same direct path for the parser's common
spellings: `complex`, `_Complex`, `complex double`, `double complex`,
`complex float`, `float complex`, and the GNU `__complex`/`__complex__`
keyword aliases. LuaJIT's parser also maps `long double complex` and matching
`_Complex`/GNU keyword variants to the existing complex-double ctype, so those
exact spellings return `CTID_COMPLEX_DOUBLE` directly too. All of these return
the existing immutable `CTID_COMPLEX_DOUBLE` or `CTID_COMPLEX_FLOAT` records,
so direct qualifiers, pointer chains, and fixed-size arrays over those bases
can reuse the same parser-free machinery.

Target primitive numeric spellings that the parser normally interns as plain
ctype records also use the direct path: `long long`, signed/unsigned
`long long` variants, and `long double`. These are not aliased to `long` or
`int64_t`; the direct path interns the same numeric ctype shape that the parser
would create on x86-64 Linux, so qualifiers, pointer chains, and fixed-size
arrays over those bases keep parser-compatible IDs.

The parser's `__int8`, `__int16`, `__int32`, `__int64`, and `__int128`
integer keyword spellings also stay off the parser token, including
`signed`/`__signed`/`__signed__` and `unsigned` before or after the keyword.
Small widths and 128-bit widths reuse immutable predefined IDs. `__int64`
uses the same parser-compatible numeric ctype shape as `long long` instead of
the x86-64 `long`/`int64_t` predefined ID, because the parser does not attach
the `long` flag to that keyword spelling.

Simple integer declaration-specifier reorderings over `char`, `short`, `int`,
`long`, and `long long` are direct too, including GNU `__signed` aliases and
suffix-style forms such as `char unsigned`, `short __signed int`, and
`long long unsigned`. The direct resolver accepts only this scalar integer
subset; duplicate or mixed specifiers still fall back to the parser for normal
diagnostics.

Exact already-published typedef identifiers and simple tag names also bypass
the parser token on the stable path. `ffi.typeof("my_typedef")`,
`ffi.sizeof("struct my_tag")`, `ffi.typeof("union my_tag")`, and similar
parameter-free APIs first resolve the name with the ctype namespace
snapshot/wait helpers and return the published ID. If another parser owns the
token, the lookup waits in native time and retries; if the string is not a
single typedef identifier or `struct`/`union`/`enum` tag lookup, the existing
parser path handles the declaration and its diagnostics.

Direct `const`/`volatile` qualifiers around one of those bases also bypass the
parser token: `ffi.typeof("const int")`, `ffi.typeof("int const")`,
`ffi.typeof("const struct my_tag")`, and `ffi.typeof("const my_typedef")`.
Scalar and void qualifiers are merged into the ctype info just like the parser;
struct/union/enum qualifiers are represented with the normal `CTA_QUAL`
attribute ctype. The GNU spellings `__const`, `__const__`, `__volatile`, and
`__volatile__` are accepted for these direct forms because the parser treats
them as the same qualifier tokens. The parser's no-op declaration tokens
`restrict`, `__restrict`, `__restrict__`, and `__extension__` are stripped in
the same direct forms.

Trailing pointer declarator chains over those direct bases also use this path:
`ffi.typeof("my_typedef **")`, `ffi.typeof("struct my_tag **")`,
`ffi.typeof("int * const")`, and `ffi.typeof("int * const * volatile")`
resolve the base without the parser token and then intern each pointer ctype
through the lock-free ctype intern table. Direct `const`/`volatile` qualifiers
after `*` are attached to that pointer ctype, matching the parser's
representation; `restrict` tokens after `*` are ignored like the parser. The
base-name wait still happens in native time when a parser is active, so the
lookup does not read rollback-sensitive names while the parser token is held.

Fixed-size array suffix chains over a direct base, including a direct pointer
chain base, also stay off the parser token: `ffi.typeof("int[4]")`,
`ffi.typeof("my_typedef[3]")`, `ffi.typeof("struct my_tag[2][3]")`, and
`ffi.typeof("my_typedef *[2][3]")`. The fast path accepts only decimal element
counts with no leading-zero C integer ambiguity, snapshots each resolved
element type's size/alignment through the ctype sequence helpers, and interns
each nested array ctype only if the element type has a stable fixed size.

The reason is concurrency, not just speed. Predefined names are installed
during `lj_ctype_init()` before the `CTState` is shared, and their payload
records are immutable. Published typedef names are not immutable in the same
way, so their fast path uses the ctype namespace snapshot/wait helpers and
falls back to the parser if the string is not a direct typedef or tag lookup.

General declarations still use the parser path. That includes unknown-size
arrays, variable-length arrays, array size expressions, array ranks beyond the
direct fast path limit, qualifiers that need attribute records,
structs/unions/enums, function types, qualified pointer chains that are not
exact predefined spellings,
declarations with `$` parameters, variable-length forms, and strings whose
internal spacing or token sequence does not exactly match the predefined table,
a single typedef identifier, a simple tag lookup, direct base qualifiers, a
trailing pointer chain over one of those bases, or a fixed-size array suffix
chain over one of those bases. Complex forms outside the exact primitive
spellings above, function pointer declarations, references, and parenthesized
declarators remain parser-owned. Those can allocate or intern ctype records,
observe rollback-sensitive names, or need normal parser diagnostics.

Validation target:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`

ffi.istype comparison snapshot
==============================

Stable `ffi.istype(ct, value)` calls compare ctype-object arguments from a
snapshot instead of directly walking the shared ctype table. The snapshot path
covers raw/ref resolution, attribute chains, qualifier collection, pointer
child compatibility, numeric/void equality, and the struct-vs-pointer special
case.

String declarations still use the parser-backed path. User-defined or
parser-created ctype comparisons still wait/retry if their sequence-checked
snapshot overlaps an active parser mutation, so rollback and in-progress type
publication remain hidden from readers.
The runtime API keeps the stock wait/retry behavior. The trace recorder
preflights snapshot-dependent comparisons and raises `CTBUSY` like the other FFI
layout/type snapshot readers instead of parking behind a parser token that is
already held.
Exact `CTypeID` equality is handled before that snapshot path. This needs no
ctype-table read, so `ffi.istype(ct, value)` can record under an unrelated
parser-owned token when the ctype object and cdata value already carry the same
ID.

Predefined immutable ctype pairs (`int`, `uint8_t`, ctype objects for those
IDs, and other records in the predefined range) now use the same comparison
body without waiting for an unrelated active parser token. The predefined path
is deliberately narrow: any comparison that walks outside the initialized
predefined range falls back to the sequence-checked retry path.

Coverage:

- added `tests/t-ffi-istype-snapshot.c` for stable scalar, ctype-object,
  struct, pointer, and struct-pointer comparisons that must not advance
  `CTState.parse_token`;
- kept a string declaration case in the same fixture to assert the parser path
  still advances the sequence;
- holds the parser token while checking predefined `int`/`uint8_t`
  comparisons, proving that immutable predefined comparisons do not park behind
  unrelated parser work;
- keeps a user-defined struct-vs-pointer comparison under a held parser token,
  proving the guarded wait/retry path still runs for parser-created records
  when the comparison needs the snapshot walker;
- holds the parser token from a trace-start callback around an exact-ID
  user-defined struct comparison and requires a successful trace with no
  `CTBUSY`;
- holds the parser token from a trace-start callback around a user-defined
  struct-vs-pointer comparison and requires a `CTBUSY` trace abort before the
  runtime wait path can park the recorder;
- wired the fixture into `m7_ffi_typeinfo_snapshot`.

Validation:

- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- stock `tests/stock/test/lib/ffi/istype.lua`
- `tools/ci/lua_test.sh m7_ffi_cparse_rollback`
- `tools/ci/lua_test.sh m7_ffi`

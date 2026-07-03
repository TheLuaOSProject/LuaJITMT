# FFI Istype Compatibility Helper Loads

`ffi_typecmp_compatptr()` and `ffi_istype_snapshot()` now use helper-backed
local `CType.info` / `CType.size` snapshots while making `ffi.istype()`
compatibility decisions.

The covered decisions include pointer qualifier compatibility, numeric/void
equivalence, recursive pointer compatibility, raw-ref id equality, and
struct-vs-pointer comparison.

Coverage:

- `m7_ffi_typeinfo_snapshot` and the stock `lib/ffi/istype.lua` case exercise
  these compatibility helpers.
- Local `CType` copies in `ffi.istype()` compatibility decisions must use the
  documented helper-load surface; that rule lives here and beside the helpers
  instead of in a source-text predicate.

Validation:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- stock `lib/ffi/istype.lua`
- `git diff --check`

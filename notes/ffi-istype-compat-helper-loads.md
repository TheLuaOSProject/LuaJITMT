# FFI Istype Compatibility Helper Loads

`ffi_typecmp_compatptr()` and `ffi_istype_snapshot()` now use helper-backed
local `CType.info` / `CType.size` snapshots while making `ffi.istype()`
compatibility decisions.

The covered decisions include pointer qualifier compatibility, numeric/void
equivalence, recursive pointer compatibility, raw-ref id equality, and
struct-vs-pointer comparison.

Guardrail:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh` rejects direct local
  `ct1.info` / `ct2.info` / `d.info` / `s.info` and matching size reads in
  these compatibility helpers.

Validation:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- stock `lib/ffi/istype.lua`
- `git diff --check`

# Recorded FFI Library Helper Loads

The remaining recorded FFI library metadata helpers in `lj_crecord.c` now avoid
raw shared `CType.info` / `CType.size` reads.

Converted paths:

- `recff_ffi_fill()` snapshots destination ctype metadata before pointer
  alignment resolution.
- `recff_ffi_xof()` snapshots the queried ctype metadata before rejecting
  variable-length `ffi.sizeof()`.
- `crec_bit64_type()` snapshots enum child metadata and size before selecting
  signed vs. unsigned 64-bit cdata rank.
- `lj_crecord_tonumber()` snapshots enum child metadata and size before
  selecting int32 vs. double conversion.

Guardrail:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh` rejects raw `->info` / `->size` reads
  in these recorded FFI library metadata helpers.

Validation:

- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- stock `lib/ffi/jit_misc.lua`
- stock `lib/ffi/bit64.lua`
- stock `lib/ffi/ffi_jit_conv.lua`
- `tools/ci/m7_ffi_carith_l.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`

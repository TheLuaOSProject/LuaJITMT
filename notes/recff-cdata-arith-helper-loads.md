# Recorded Cdata Arithmetic Helper Loads

`recff_cdata_arith()` now snapshots operand ctype metadata through
`ctype_info_acq()` / `ctype_size_acq()` while normalizing operands before
dispatching to the int64, pointer, or metamethod recorder helpers.

The helper-backed snapshots cover cdata pointer/reference resolution, enum child
resolution, function cdata normalization to pointers, numeric payload loads,
string-to-enum lookup fallback, and string-to-pointer decay. The locked enum
fallback now reads matched constant metadata with helper loads as well.

Guardrail:

- `tools/ci/m7_ffi_carith_l.sh` rejects raw `->info` / `->size` reads in
  `recff_cdata_arith()`.

Validation:

- `tools/ci/m7_ffi_carith_l.sh`
- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `tools/ci/m7_ffi_jit_cnew.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`

# Recorder Pointer Arithmetic Helper Loads

`crec_arith_ptr()` now snapshots pointer and index operand ctype metadata through
`ctype_info_acq()` before recording pointer arithmetic or comparisons.

The helper already used `lj_ctype_size_snapshot()` for pointed-to element size.
This slice removes the remaining direct `CType.info` reads from pointer/refarray
classification, pointer-pointer difference and comparison checks, numeric index
validation, swapped `number + pointer` handling, fallback element-size reads,
and result pointer ctype interning.

Guardrail:

- `m7_ffi_carith_l` invariant: raw `->info` / `->size` reads in
  `crec_arith_ptr()`.

Validation:

- `tools/ci/m7_ffi_carith_l.sh`
- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `tools/ci/m7_ffi_jit_cnew.sh`
- `git diff --check`

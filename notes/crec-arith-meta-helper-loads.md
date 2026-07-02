# Recorder Arithmetic Metamethod Helper Loads

`crec_arith_meta()` now snapshots operand ctype metadata through
`ctype_info_acq()` before arithmetic metatype lookup and fallback pointer
equality classification.

This removes direct `CType.info` reads from:

- first-operand arithmetic metatype lookup;
- second-operand lookup when the first operand has no function metamethod;
- cdata pointer equality fallback that checks whether both operands share the
  numeric/non-numeric class.

Guardrail:

- `m7_ffi_carith_l` invariant: raw `->info` / `->size` reads in
  `crec_arith_meta()`.

Validation:

- `tools/ci/m7_ffi_carith_l.sh`
- `tools/ci/m7_ffi_metatype.sh`
- `tools/ci/m7_ffi_jit_cnew.sh`
- `git diff --check`

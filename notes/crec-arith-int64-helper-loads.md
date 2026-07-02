# Recorder Int64 Arithmetic Helper Loads

`crec_arith_int64()` now snapshots both operand ctypes through
`ctype_info_acq()` / `ctype_size_acq()` before recording int64 cdata arithmetic
or comparisons.

The snapshots drive numeric operand checks, unsigned 64-bit result selection,
32-bit comparison narrowing, and signed versus unsigned extension of integer
operands. This removes direct shared `CType.info` / `CType.size` reads from the
recorder's int64 arithmetic helper.

Guardrail:

- `m7_ffi_carith_l` invariant: raw `->info` / `->size` reads in
  `crec_arith_int64()`.

Validation:

- `tools/ci/m7_ffi_carith_l.sh`
- `tools/ci/m7_ffi_jit_cnew.sh`
- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `git diff --check`

# JIT CALLX Helper Loads

`asm_callx_flags()` now snapshots the recorded function ctype metadata with
`ctype_info_acq()` before reconstructing `CALLX*` call flags.

The helper-backed snapshot covers vararg flag reconstruction and x86 calling
convention bits used by traced FFI calls.

Guardrail:

- `tools/ci/m7_ffi_cdata_set_l.sh` rejects raw `->info` / `->size` reads in
  `asm_callx_flags()`.

Validation:

- `tools/ci/m7_ffi_cdata_set_l.sh`
- direct `-jdump=ir` traced `ffi.C.abs()` / vararg `ffi.C.snprintf()` probe
  with `CALLXS` IR and `callx ok`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`

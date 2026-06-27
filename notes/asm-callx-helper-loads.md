# JIT CALLX Helper Loads

`asm_callx_flags()` now snapshots the recorded function ctype metadata with
`ctype_info_acq()` before reconstructing `CALLX*` call flags.

The helper-backed snapshot covers vararg flag reconstruction and x86 calling
convention bits used by traced FFI calls. Default builds no longer record FFI
C calls until `IR_CALLXS` has a native-state protocol; this helper remains for
explicit `LJ_FFI_RECORD_CALLS` performance experiments and the eventual native
bridge.

Guardrail:

- `tools/ci/m7_ffi_cdata_set_l.sh` rejects raw `->info` / `->size` reads in
  `asm_callx_flags()`.

Validation:

- `tools/ci/m7_ffi_cdata_set_l.sh`
- Historical direct `-jdump=ir` traced `ffi.C.abs()` / vararg
  `ffi.C.snprintf()` probe with `CALLXS` IR and `callx ok`; default validation
  now expects ordinary FFI C calls to stay off trace.
- `tools/ci/m0_source_guard.sh`
- `git diff --check`

# JIT CALLX Helper Loads

`asm_callx_flags()` now snapshots the recorded function ctype metadata with
`ctype_info_acq()` before reconstructing `CALLX*` call flags.

The helper-backed snapshot covers vararg flag reconstruction and x86 calling
convention bits used by traced FFI calls. Broad generic `IR_CALLXS` recording
still stays interpreted because it lacks a native-state protocol; that boundary
is documented in `lj_crecord.c` instead of enforced by an opt-in build check.
This helper remains for the eventual native bridge.

Coverage model:

- Active coverage stays in `m7_ffi_cdata_set_l` behavior/counter fixtures. Direct helper/backend sites carry comments for the ordering rationale.

Validation:

- `tools/ci/m7_ffi_cdata_set_l.sh`
- Historical direct `-jdump=ir` traced `ffi.C.abs()` / vararg
  `ffi.C.snprintf()` probe with `CALLXS` IR and `callx ok`; default validation
  now expects ordinary FFI C calls to stay off trace.
- `git diff --check`

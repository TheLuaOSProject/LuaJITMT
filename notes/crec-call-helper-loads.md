# Recorder C-call Helper Loads

`crec_call()` now snapshots function and return `CType.info` / `CType.size`
through helper-backed acquire loads before recording a traced C call.

The helper uses these snapshots to resolve function pointers, select the
pointer IR type, classify the C return type, decide whether the return value is
void/number/pointer/enum, and box pointer, enum, and 64-bit integer results.
The snapshots are taken before `crec_call_args()` can invalidate local CType
pointers by inferring vararg ctypes.

Guardrail:

- `tools/ci/m7_ffi_cdata_set_l.sh` rejects raw `->info` / `->size` reads in
  `crec_call()`.

Validation:

- `tools/ci/m7_ffi_cdata_set_l.sh`
- `tools/ci/m7_ffi_jit_cnew.sh`
- `tools/ci/m7_ffi_callback_runtime.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`

# Recorded Cdata Call Helper Loads

`recff_cdata_call()` now snapshots the resolved raw ctype with
`ctype_info_acq()` before selecting the id used for recorded `__call` / `__new`
metatype lookup.

This removes the remaining direct `CType.info` read from the recorded cdata call
path after constructor handling and direct C-call recording decline to handle
the call.

Coverage model:

- Active coverage stays in `m7_ffi_metatype` behavior/counter fixtures. Direct helper/backend sites carry comments for the ordering rationale.

Validation:

- `tools/ci/m7_ffi_metatype.sh`
- `tools/ci/m7_ffi_jit_cnew.sh`
- `git diff --check`

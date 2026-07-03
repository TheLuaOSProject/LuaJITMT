crec_ct2irt helper loads

- Routed the JIT recorder's `crec_ct2irt()` helper through
  `ctype_info_acq()`/`ctype_size_acq()` before deciding numeric, pointer, enum,
  and complex IR types.
- Refreshed the helper-backed info snapshot after enum child resolution so the
  final IR mapping uses the resolved child ctype payload.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_jit_cnew` behavior/counter fixtures and code-adjacent helper docs; raw-field implementation-text inventories are not pass/fail contracts.

Verification:

- `tools/ci/m7_ffi_jit_cnew.sh`
- `tools/ci/m7_ffi_snap_restore_l.sh`
- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `git diff --check`

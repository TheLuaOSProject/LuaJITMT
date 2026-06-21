crec_ct2irt helper loads

- Routed the JIT recorder's `crec_ct2irt()` helper through
  `ctype_info_acq()`/`ctype_size_acq()` before deciding numeric, pointer, enum,
  and complex IR types.
- Refreshed the helper-backed info snapshot after enum child resolution so the
  final IR mapping uses the resolved child ctype payload.
- Extended `tools/ci/m7_ffi_jit_cnew.sh` to reject raw `CType.info` and
  `CType.size` reads in `crec_ct2irt()`.

Verification:

- `tools/ci/m7_ffi_jit_cnew.sh`
- `tools/ci/m7_ffi_snap_restore_l.sh`
- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`

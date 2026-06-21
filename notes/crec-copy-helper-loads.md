crec aggregate copy helper loads

- Routed `crec_copy_struct()` through helper-backed field sibling, info, and
  size snapshots while building struct copy plans for named scalar fields and
  complex field halves.
- Routed `crec_copy()` aggregate dispatch through a helper-backed info snapshot
  before choosing array, struct, union, and raw-copy behavior.
- Extended `tools/ci/m7_ffi_jit_cnew.sh` to reject raw `CType.info`,
  `CType.size`, and `CType.sib` reads in the guarded aggregate copy planner.

Verification:

- `tools/ci/m7_ffi_jit_cnew.sh`
- `tools/ci/m7_ffi_snap_restore_l.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`

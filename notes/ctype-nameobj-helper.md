CType name object helper

- Added `ctype_nameobj_acq()` for snapshot paths that need the raw `GCobj *`
  name edge rather than the existing `GCstr *` returned by `ctype_name_acq()`.
- Routed ctype snapshots, name lookup snapshots, field snapshots, enum-constant
  snapshots, and FFI typeinfo/layout snapshots through the helper instead of
  direct `gcref_acq(ct->name)` loads.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_typeinfo_snapshot` behavior/counter fixtures; the helper comments carry the ordering rationale.

Verification:

- tools/ci/m7_ffi_typeinfo_snapshot.sh
- tools/ci/m7_ffi_cparse_rollback.sh
- tools/ci/m7_ffi_ctype_name_claim.sh
- git diff --check

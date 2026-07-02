CType payload helpers

- Added `ctype_info_acq()` and `ctype_size_acq()` for acquire-loading the
  immutable ctype payload fields after a table/header snapshot.
- Routed ctype snapshots, name/field/size/enum snapshots, and FFI
  typeinfo/typecmp/layout snapshots through the helper API instead of raw
  `la_load32_acq(&ct->info)` / `la_load32_acq(&ct->size)` calls.
- Documented the invariant formerly checked by `m7_ffi_typeinfo_snapshot`: raw shared
  `CType.info`/`CType.size` acquire-loads in the guarded ctype/FFI/parser
  files.

Verification:

- tools/ci/m7_ffi_typeinfo_snapshot.sh
- tools/ci/m7_ffi_cparse_rollback.sh
- tools/ci/m7_ffi_ctype_name_claim.sh
- git diff --check

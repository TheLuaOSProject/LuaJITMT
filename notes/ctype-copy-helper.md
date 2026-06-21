CType stale-slot copy helper

- Added `ctype_info_rel()`, `ctype_size_rel()`, `ctype_nameobj_rel()`, and
  `ctype_copy_rel()` to mirror the existing acquire helper surface with
  release-published ctype payload/name copies.
- Replaced raw `*dst = *src` stale-slot refreshes in `ctype_hash_setnext()`,
  `ctype_publish_current()`, and parser `cp_ctype_publish()` with
  `ctype_copy_rel()`.
- Extended `tools/ci/m7_ffi_ctype_name_claim.sh` to reject raw ctype
  stale-slot structure copies in the ctype/parser publication paths.

Verification:

- tools/ci/m7_ffi_ctype_name_claim.sh
- tools/ci/m7_ffi_cparse_rollback.sh
- tools/ci/m7_ffi_ctype_ticket_intern.sh
- tools/ci/m7_ffi_ctype_tab_retire.sh
- tools/ci/m7_ffi_typeinfo_snapshot.sh
- tools/ci/m0_source_guard.sh
- git diff --check

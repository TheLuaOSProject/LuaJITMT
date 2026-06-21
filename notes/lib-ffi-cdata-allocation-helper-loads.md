lib_ffi cdata allocation helper loads

- Routed `ffi_new` post-initialization struct finalizer checks through
  `ctype_info_acq()`.
- Routed `ffi_cast` parser-fallback and string-ctype paths through
  `ctype_info_acq()`/`ctype_size_acq()` snapshots before validation and cdata
  allocation.
- Reused the helper-backed size snapshot for cast destination cdata allocation.
- Extended `tools/ci/m7_ffi_cdata_alloc.sh` to reject raw `CType.info` and
  `CType.size` reads in these allocation helper bodies.

Verification:

- `tools/ci/m7_ffi_cdata_alloc.sh`
- `tools/ci/m7_ffi_cdata_set_l.sh`
- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `tools/ci/m7_ffi_callback_install.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`

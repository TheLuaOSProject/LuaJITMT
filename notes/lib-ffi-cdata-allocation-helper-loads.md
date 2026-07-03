lib_ffi cdata allocation helper loads

- Routed `ffi_new` post-initialization struct finalizer checks through the
  layout snapshot info already used for allocation, so the path no longer
  refetches the raw ctype table slot after cdata allocation.
- Routed `ffi_cast` parser-fallback and string-ctype paths through
  `ctype_info_acq()`/`ctype_size_acq()` snapshots before validation and cdata
  allocation.
- Reused the helper-backed size snapshot for cast destination cdata allocation.
- Documented why this shared state is owned by the helper surface. Active coverage stays in `m7_ffi_cdata_alloc` behavior/counter fixtures and code-adjacent helper docs; raw-field implementation-text inventories are not pass/fail contracts.

Verification:

- `tools/ci/m7_ffi_cdata_alloc.sh`
- `tools/ci/m7_ffi_cdata_set_l.sh`
- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `tools/ci/m7_ffi_callback_install.sh`
- `git diff --check`

# Recorded C Library Namespace Helper Loads

`recff_clib_index()` now snapshots cached `ffi.C` symbol metadata with
`ctype_info_acq()` / `ctype_size_acq()` before recording namespace constants or
extern symbol conversions.

The helper-backed snapshots cover constant-value detection, large unsigned
constant widening to a numeric IR constant, and extern ctype id selection before
the recorder converts a cached cdata symbol into the requested value form.

Guardrail:

- `tools/ci/m7_ffi_clib_cache.sh` rejects raw `->info` / `->size` reads in
  `recff_clib_index()`.

Validation:

- `tools/ci/m7_ffi_clib_cache.sh`
- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `tools/ci/m7_ffi_jit_cnew.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`

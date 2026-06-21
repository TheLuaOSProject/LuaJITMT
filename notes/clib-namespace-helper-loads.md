# C Library Namespace Helper Loads

Runtime `ffi.C` symbol resolution now avoids raw shared `CType` payload reads
in `src/lj_clib.c`.

Converted paths:

- `clib_func_argsize()` walks function argument fields through
  `ctype_sib_acq()`, `ctype_info_acq()`, and `ctype_size_acq()`.
- `clib_extsym()` reads redirect attributes through helper-backed sibling and
  info snapshots.
- `lj_clib_index()` snapshots parser-locked fallback lookups into a local
  `CType`, then uses helper-backed metadata for constants, unsigned widening,
  extern/function classification, and x86 decorated symbol fallback.

Guardrail:

- `tools/ci/m7_ffi_clib_cache.sh` rejects raw `->info`, `->size`, or `->sib`
  reads in these runtime C library namespace helpers.

Validation:

- `tools/ci/m7_ffi_clib_cache.sh`
- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`

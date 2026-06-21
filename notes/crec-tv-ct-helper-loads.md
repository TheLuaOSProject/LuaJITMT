# JIT C-to-TValue Load Helper Loads

`crec_tv_ct()` now snapshots source `CType.info` and `CType.size` through
`ctype_info_acq()` / `ctype_size_acq()` after mapping the ctype to an IR type.

The helper uses those snapshots when recording C-data loads back to Lua values:
numeric loads, bool guard specialization, pointer and enum boxing, reference
array/struct boxing, and complex-number half-size planning. This avoids direct
shared ctype payload reads in another recorder conversion helper while CTState
read paths move toward lockless publication.

Guardrail:

- `tools/ci/m7_ffi_jit_cnew.sh` rejects raw `->info` / `->size` reads in
  `crec_tv_ct()`.

Validation:

- `tools/ci/m7_ffi_jit_cnew.sh`
- `tools/ci/m7_ffi_cdata_get_l.sh`
- `tools/ci/m7_ffi_typeinfo_snapshot.sh`
- `tools/ci/m0_source_guard.sh`
- `git diff --check`

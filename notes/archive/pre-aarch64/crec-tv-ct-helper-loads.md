# JIT C-to-TValue Load Helper Loads

`crec_tv_ct()` now snapshots source `CType.info` and `CType.size` through
`ctype_info_acq()` / `ctype_size_acq()` after mapping the ctype to an IR type.
Enum child resolution uses the recorder snapshot helper, so trace recording
aborts with `CTBUSY` instead of walking the live ctype table if an active parser
mutation window is observed.

The helper uses those snapshots when recording C-data loads back to Lua values:
numeric loads, bool guard specialization, pointer and enum boxing, reference
array/struct boxing, and complex-number half-size planning. This avoids direct
shared ctype payload reads in another recorder conversion helper while CTState
read paths move toward lockless publication.

Behavior coverage lives in `tests/t-ffi-recorder-libmeta-busy.c`, which records
cdata enum loads while the parser token is held and verifies ordinary enum
field/array reads in a hot loop after the abort.

Validation:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/lua_test.sh m7_ffi_cdata_get_l`
- `tools/ci/lua_test.sh m7_ffi_jit_cnew`
- `git diff --check`

# JIT TValue-to-C Store Helper Loads

`crec_ct_tv()` snapshots destination and source ctype metadata through recorder
helpers instead of reading shared ctype payload fields directly.

The destination snapshots cover string-to-enum matching, string-to-array
rejection, and final enum child resolution before dispatching to `crec_ct_ct()`.
The source snapshots are refreshed after cdata source normalization,
function-to-pointer interning, reference child resolution, and enum child
resolution so unboxing and conversion planning observe helper-backed payloads.
The enum string fallback now snapshots matched constant value metadata before
using `ctype_size_acq()`. If the ctype parser is publishing, the recorder
aborts with `CTBUSY` instead of walking the live ctype table.

Invariant check:

- `tests/t-ffi-recorder-libmeta-busy.c` covers enum string stores, cdata
  arithmetic/conversion sources, aggregate copies, and fixed C calls while a
  held parser token forces recorder `CTBUSY` aborts.

Validation:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/lua_test.sh m7_ffi_cdata_set_l`
- `tools/ci/lua_test.sh m7_ffi_jit_cnew`
- `git diff --check`

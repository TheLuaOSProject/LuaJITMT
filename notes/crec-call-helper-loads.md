# Recorder C-call Helper Loads

`crec_call()` snapshots function and return ctype metadata before recording a
traced C call. Function-pointer normalization and return-child resolution now
use the recorder snapshot helpers, so an active parser publish aborts the trace
with `CTBUSY` instead of walking the live ctype table.

The helper uses these snapshots to resolve function pointers, select the
pointer IR type, classify the C return type, decide whether the return value is
void/number/pointer/enum, and box pointer, enum, and 64-bit integer results.
The snapshots are taken before `crec_call_args()` can invalidate local CType
pointers by inferring vararg ctypes.

Guardrail:

- `tests/t-ffi-recorder-libmeta-busy.c` covers traced C calls under a held
  ctype parser token and the normal hot-loop path that must still record.

Validation:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/lua_test.sh m7_ffi_cdata_set_l`
- `tools/ci/lua_test.sh m7_ffi_callback_runtime`
- `git diff --check`

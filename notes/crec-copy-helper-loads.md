crec aggregate copy helper loads

- Routed `crec_copy_struct()` through helper-backed field sibling, info, and
  size snapshots while building struct copy plans for named scalar fields and
  complex field halves.
- Routed `crec_copy()` array element and struct field raw-child traversal
  through recorder-local snapshots. These paths now abort the trace with
  `CTBUSY` if aggregate copy planning races an active parser mutation window
  instead of waiting on the parser token.
- Extended `tests/t-ffi-recorder-libmeta-busy.c` with struct and array
  assignment cases that hold the parser token during trace recording and then
  verify normal hot-loop aggregate copy behavior after the abort.

Verification:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/lua_test.sh m7_ffi_jit_cnew`
- `tools/ci/lua_test.sh m7_ffi_carith_l`
- `git diff --check`

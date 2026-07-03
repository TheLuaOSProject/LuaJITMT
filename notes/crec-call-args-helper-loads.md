# Recorder C-call Argument Helper Loads

`crec_call_args()` snapshots function and argument ctype metadata before using
`ctype_info_acq()`, `ctype_size_acq()`, and `ctype_sib_acq()` instead of
walking live `CType` payloads directly.

The helper uses those snapshots for initial parameter attribute skipping,
argument field traversal, vararg inferred argument type validation, integer
promotion and sign-extension decisions, soft-float split checks, and ABI
register accounting. Argument metadata is snapped before `crec_ct_tv()` so
later conversion and vararg inference cannot invalidate a `CType *` and then
leave subsequent recorder decisions reading the stale payload directly. If the
parser is publishing, the recorder aborts with `CTBUSY`.

Invariant check:

- `tests/t-ffi-recorder-libmeta-busy.c` covers traced fixed-argument C calls
  under a held ctype parser token and the normal hot-loop path that must still
  record.

Validation:

- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/lua_test.sh m7_ffi_cdata_set_l`
- `tools/ci/lua_test.sh m7_ffi_callback_runtime`
- `git diff --check`

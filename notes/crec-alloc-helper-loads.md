# Recorder Cdata Allocation Helper Loads

`crec_alloc()` uses helper-backed ctype metadata snapshots while recording
trace-side cdata allocation and aggregate initialization.

The helper already starts from `lj_ctype_info_snapshot()` for the root ctype.
The aggregate initializer follow-up also snapshots array element ctypes,
struct field nodes, field child ctypes, and exact-cdata initializer source
types before using `ctype_info_acq()`, `ctype_size_acq()`, and
`ctype_sib_acq()` for element types, field lists, named-field checks, field
offsets, union size decisions, exact aggregate copy decisions, and aggregate
fallback selection. If a parser publish is active, the recorder aborts with
`CTBUSY` instead of touching live ctype payloads or waiting.

Invariant check:

- `tests/t-ffi-recorder-libmeta-busy.c` now covers traced struct and array
  aggregate `ffi.new` initializers, including exact aggregate copy
  constructors, under parser-busy trace hooks, plus the normal hot-loop path
  that should still record successfully.

Validation:

- `tools/ci/lua_test.sh m7_ffi_jit_cnew`
- `tools/ci/lua_test.sh m7_ffi_cdata_alloc`
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `git diff --check`

# FFI C library extern snapshots

Cached `ffi.C` and `ffi.load()` extern variable reads/writes now snapshot the
extern wrapper ctype and the child value ctype before converting between C
memory and Lua values. This removes direct shared ctype table reads from the
post-lookup `ffi_clib___index` and `ffi_clib___newindex` paths.

The conversion helpers still receive the raw child ctype and id, preserving the
existing numeric, enum, pointer, aggregate, and const-write behavior. Exact
struct/union copies now compare raw ctype ids rather than `CType *` identity,
so a sequence-checked stack copy of the destination ctype behaves like the
table-backed record it was copied from. Const qualification is taken from
`lj_ctype_info_snapshot()`, so attribute chains are still honored without
reacquiring the parser token on stable published ctypes.

Coverage:
- `tests/t-ffi-clib-extern-snapshot.c` loads a small shared library with a
  mutable extern integer, a mutable extern struct, and a const extern integer.
- The fixture verifies stable extern reads/writes do not advance
  `CTState.parse_token`, const writes still fail, and cached extern read/write
  operations wait from a native region while the parser token is held.
- The fixture is wired into `m7_ffi_clib_cache`; this is behavior coverage,
  not a legacy wrapper.

# Progress Report - 2026-06-28 Compatibility And Lock Update

Scope: stock compatibility, FFI parser fallback reduction, and coordination
inventory.

## Landed In This Area

- Recorder string ctype parsing aborts with `CTBUSY` while another parser owns
  the mutation token, then records normally after release.
- Parser rollback paths moved further toward local copy-then-publish so fewer
  shared `CType` slots are mutated in place.
- `ffi.istype()` for stable non-string comparisons retries sequence-checked
  snapshots instead of taking the parser token itself.
- Enum string constant conversion uses sequence-checked snapshot wait/refetch
  for hits and misses.
- Active-token behavior coverage exists for string ctype recorder paths,
  `ffi.istype()`, and enum conversion.

## Remaining Locks Outside Mutable `ffi.cdef`

- Interpreter FFI layout/string paths where rollback, abandoned entries, VLA/VLS
  size, or error construction still need conservative handling.
- Per-state owner claims.
- Threading mutex/channel/join waits.
- Safepoint leadership and GC2 worker lifecycle waits.
- GDBJIT publication lock.

## Verification At The Time

- `make -C src -j`
- `tools/ci/lua_test.sh m7_ffi_typeinfo_snapshot`
- `tools/ci/lua_test.sh m7_ffi_cparse_rollback`
- `tools/ci/lua_test.sh m7_ffi_carith_l m7_ffi_ctype_tab_retire m7_ffi_cparse_rollback`
- Stock base/meta/library subsets under the built VM.

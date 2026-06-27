## CTypeTab retired_next acquire/release helper

Slice: FFI CType table retirement link discipline.

Changes:
- Added `ctype_tab_retired_next_acq()` and `ctype_tab_retired_next_rel()` in
  `src/lj_ctype.h` beside `CTypeTab`.
- Routed CType table allocation initialization, retired-list push, epoch reclaim,
  and final free through the helper in `src/lj_ctype.c`.
- Routed legacy GC, GC2, paranoia raw-root scans, and the focused retirement C
  test through the same acquire reader helper.
- Extended `tools/ci/m7_ffi_ctype_tab_retire.sh` to reject direct
  `CTypeTab.retired_next` access in the ctype retirement path and GC walkers.
- Left `CTState.retiredtab` head operations as explicit acquire/CAS/xchg sites;
  this slice only centralizes the per-node retired link.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m7_ffi_ctype_tab_retire.sh`
- `tools/ci/m9_gc_stats.sh`
- `tools/ci/m7_ffi.sh`

Follow-up:
- The analogous `StrTabHdr.retired_next` helper cleanup is covered in
  `notes/strtab-retired-next-atomic.md`; that path now also centralizes the
  string-table heads and `StrTabHdr.retire_epoch`.

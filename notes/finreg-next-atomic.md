## FINREG generation/order next-link helpers

Slice: FFI finalizer registry generation and ordered-registration link
discipline.

Changes:
- Added `fin_gen_next_acq/rel()` and `fin_order_next_acq/rel()` in
  `src/lj_ctype.h` beside `FinRegGen` and `FinRegOrderNode`.
- Routed FINREG generation creation, CAS publish, lookup scans, claim scans,
  FINREG table checks, mark/free paths, and ordered registration publish/free
  through the helpers.
- Routed GC ordered cdata finalizer scans and FINREG disable scans through the
  same helpers.
- Extended `tools/ci/m7_ffi_finreg.sh` to reject direct `gen->next` or
  `ord->next` access in `lj_ctype.c` and `lj_gc.c`.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/m7_ffi_finreg.sh`
- `tools/ci/m9_gc_stats.sh`

Notes:
- `CTState.fin_head` and `CTState.fin_order_head` head operations remain
  explicit acquire/CAS/xchg sites. This slice centralizes only the per-node
  next links.

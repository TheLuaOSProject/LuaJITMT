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
- Documented the rule that FINREG generation/order next links are shared
  publication links and must be accessed through the acquire/release helpers.
  Runtime and GC fixtures exercise creation, lookup, marking, and disable
  paths; CI must not enforce the helper spelling by repository text assertion.

Validation:
- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/lua_test.sh m7_ffi_finreg`
- `tools/ci/m9_gc_stats.sh`

Notes:
- `CTState.fin_head` and `CTState.fin_order_head` head operations remain
  explicit acquire/CAS/xchg sites. This slice centralizes only the per-node
  next links.

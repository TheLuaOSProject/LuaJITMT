# FINREG Order Node Validation

FINREG cdata finalizer order nodes are raw side-list records. The active and
retired order lists can retain stale words after logical retire/splice races, so
walkers must prove the node record is still registered memory before reading
`next`, `retired_next`, `active`, `obj`, `tab`, or `slot`.

This slice adds that guard to:

- `lj_ctype_fin_order_retire()` before touching an order node or predecessor;
- `lj_ctype_fin_order_retire_obj()`;
- `lj_ctype_fin_mark()` active and retired order roots;
- close-time `lj_ctype_fin_freetabs()`;
- GC2 cdata finalizer order scans for P_WEAK, close, and pending checks;
- the focused `t-gc2-traverse` helper that manually counts active order refs.

Validation:

- `make -C src -j$(nproc) TARGET_STRIP=:`
- `tools/ci/lua_test.sh m8_weak`
- `tools/ci/lua_test.sh m3_gc2_paranoia`
- `tools/ci/lua_test.sh m0_matrix`

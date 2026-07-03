M6 helper-backed NEWREF key resolution:
- Strengthened `lj_tab_storetv_forjit_newref()` so it resolves
  `lj_tab_set(L, parent, key)` before every CAS instead of first attempting the
  recorder-returned slot. This is deliberately narrower than the final
  generated RETIRING/FORWARD/CAS protocol, but it closes the stale retiring
  generation case for traced fresh hash/array slots because `IR_NEWREF` already
  passes the key to the helper.
- The weak-write bridge now receives that key for NEWREF stores instead of
  `NULL`, preserving weak-key marking for helper-backed fresh hash stores.
- `tests/t-jit-forward-store.c` now covers retiring-but-not-forwarded old array
  and hash slots. The old helper would publish into those stale slots; the new
  helper keeps them nil and writes through the current generation.
- Invariant check: `tools/ci/m6_jit_table_store_helper.sh` checks
  resolve-before-CAS ordering, the key-aware weak bridge, and both retiring
  regression cases.

Follow-up: key-aware ASTORE/HSTORE helper routing:
- Extended the same idea to existing helper-backed array/hash stores.
  `lj_tab_storetv_forjit_array()` now takes the raw array index, and
  `lj_tab_storetv_forjit_hash()` takes the traced TValue key. They keep the old
  fast path only when the recorded slot is still inside the parent's current
  generation; otherwise they resolve by index/key before CAS.
- x64 `asm_ahstore_forjit()` now passes the raw AREF index directly for array
  stores and materializes a second temporary TValue for HREF, HREFK, and NEWREF
  keys.
- `tests/t-jit-forward-store.c` has retiring-but-not-forwarded existing
  ASTORE/HSTORE cases in addition to the earlier NEWREF cases.

Follow-up: current-but-retiring generation helper routing:
- `tab_current_jit_array_slot()` and `tab_current_jit_hash_slot()` now use
  helper-local raw acquire snapshots instead of the generic non-retiring
  snapshot helpers. This lets a helper-backed store observe the narrow resize
  window where `parent->array` or `parent->node` still names the old generation
  after `next_gen` and `RETIRING` have been published.
- Existing ASTORE/HSTORE helpers now hop through the published `next_gen` even
  when the old slot still contains an ordinary value rather than `LJ_TFORWARD`.
  The old helper path could spin in the snapshot helper or CAS-publish into the
  retiring generation.
- `tests/t-jit-forward-store.c` now has array/hash cases that temporarily
  restore the table mirror to a RETIRING generation and assert the old slot is
  unchanged while the next generation receives the helper store.

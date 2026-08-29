JIT table-store observed generation routing

- `tab_forwarded_jit_array_slot()` and `tab_forwarded_jit_hash_slot()` now
  resolve stale slots using the array/node generation already observed by
  `tab_current_jit_*_slot()`.
- This closes the race where the parent table mirror can advance between the
  pointer-in-current-generation check and the forwarded-slot resolver. The old
  helpers could re-snapshot the parent or fall through to the old destination
  and publish into a retired generation.
- The helpers now also recheck the observed generation's retiring bit before
  treating an ordinary old value as writable in place.
- If the successor slot is not found after recovering the key, the helper falls
  back to `lj_tab_set()` on the current parent table instead of handing the
  stale forwarded slot back to the CAS loop.
- Added `LJ_TAB_TEST_HELPERS` coverage in `t-jit-forward-store.c` for an
  observed old array/hash generation whose parent mirror is already current,
  with both `FORWARD` and ordinary retiring old values.

Validation:

- `tools/ci/m6_jit_table_store_helper.sh`
- `tools/ci/lua_test.sh m5_tab_cas_store`

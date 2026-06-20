# Table unpublished hash-key filtering

Date: 2026-06-20

## Problem

Lockless table insertion can briefly hold a non-nil hash value before the key
is publish-visible. FINREG empty-anchor insertion uses this shape while
claiming a value slot and then publishing the cdata key.

Hash scanners treated non-nil values as visible without first requiring a
published key. That could expose a nil-key placeholder through `next()` or make
resize/rehash accounting treat an unpublished slot as a real entry.

## Fix

- Added a local `tab_hash_key_hidden()` predicate for nil and `KEYLOCK` hash
  keys.
- `lj_tab_resize()` now skips hidden-key hash slots when copying old hash
  generations.
- `tab_rehash_hashcount()` and `counthash()` no longer count hidden-key slots.
- `lj_tab_next()` skips hidden-key hash slots, including `FORWARD` resolution
  paths.
- Extended `t-tab-keylock-lookup` with a nil-key/non-nil-value placeholder that
  must be invisible to `next()` and must not force a hash part during resize.

## Verification

Passed:

- `tools/ci/lua_test.sh m5_tab_keylock_lookup`
- `tools/ci/lua_test.sh m5_tab_forward_filter`
- `tools/ci/lua_test.sh m5_tab_cas_store`
- `tools/ci/lua_test.sh m7_ffi_finreg`

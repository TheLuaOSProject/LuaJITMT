# Ctype abandon publish through current table

Date: 2026-06-20

## Problem

`lj_ctype_addname_unique()` can abandon a duplicate named ctype after the ctype
table has moved. `ctype_abandon()` updated the raw pointer it was handed, which
could be a stale table entry after a reservation-triggered move. The duplicate
loser would not be marked abandoned in the current table, leaving later public
ctype lookups able to observe the wrong state.

## Fix

- Added a local `ctype_publish_current()` helper in `src/lj_ctype.c` that
  mirrors the existing parser publish loop and copies a ctype update into the
  current table slot for the target id.
- Changed `ctype_abandon()` to build an abandoned snapshot, preserve the hash
  chain `next` link, clear the name, and publish the result through the current
  table.
- Extended `t-ffi-ctype-name-claim` with a duplicate-name loser whose table
  slot is forced to move before the unique-name claim resolves; the test checks
  that the current table entry is abandoned and `ffi.typeinfo()` hides it.

## Verification

Passed:

- `tools/ci/lua_test.sh m7_ffi_ctype_name_claim`
- `tools/ci/lua_test.sh m7_ffi_ctype_ticket_intern`
- `tools/ci/lua_test.sh m7_ffi_ctype_tab_retire`

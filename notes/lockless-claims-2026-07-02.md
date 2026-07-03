# Lockless Claim Audit 2026-07-02

Two reported warm-path claims were stale in their strongest form on current
`v2.1`:

- Table resize ownership is per table, not universe-global. `GCtab.struct_owner`
  still serializes same-table structural mutation (`lj_tab_resize()`,
  active-MT `table.insert()`, and `table.clear()`), but different tables do not
  share one global owner word.
- Table readers hitting `KEYLOCK` no longer sleep for 1 ms. The shared wait
  helper retries with CPU pauses and then scheduler yield. Read-only table
  lookups now use a no-yield one-shot retry before filtering unpublished keys;
  writer/resize paths keep the wait because they still need publication or
  migration completion.
- String interning no longer takes a shared-header reader-count pin per intern.
  The hot path uses a TG-local active marker plus per-bucket CAS. Remaining
  shared contention is `g->str.id`, `g->str.num`, and resize/rehash ownership.

Current temporary bridges remain:

- Table resize is still not the final cooperative per-generation helper-copy
  protocol from `plan/06_concurrent_objects.md`.
- String resize/secondary rehash still relies on TG-local active markers because
  current resize destructively reuses `GCstr.gcw` chain links.

Focused regression tests for the table read cleanup:

- `tools/ci/lua_test.sh m5_tab_keylock_lookup m5_tab_next_snapshot m5_tab_forward_filter`

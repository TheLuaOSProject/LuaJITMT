# Lockless Claim Audit 2026-07-02

Two reported warm-path claims were stale in their strongest form on current
`v2.1`:

- Table resize ownership is per table, not universe-global. `GCtab.struct_owner`
  still serializes same-table structural mutation (`lj_tab_resize()`,
  active-MT `table.insert()`, and `table.clear()`), but different tables do not
  share one global owner word. Same-table contenders now use the regular table
  retry-yield discipline, not a fixed 1 ms timed park.
- Table readers hitting `KEYLOCK` no longer sleep for 1 ms and now avoid the
  no-`lua_State` wait helper on the read-only lookup/traversal surfaces. Direct
  string/integer getters, generic `lj_tab_get()`, and `lj_tab_next()` retry or
  snapshot and then filter unpublished keys if they remain hidden. Writer/resize
  paths keep waits where they still require publication or migration completion.
- String interning no longer takes a shared-header reader-count pin per intern.
  The hot path uses a TG-local active marker plus per-bucket CAS. Remaining
  shared contention is `g->str.id`, `g->str.num`, and resize/rehash ownership.
  Legacy string sweep now also claims the current header before destructive
  bucket relink/free, so sweep does not race active lockless intern walks on the
  current bridge.

Current temporary bridges remain:

- Table resize is still not the final cooperative per-generation helper-copy
  protocol from `plan/06_concurrent_objects.md`.
- String resize/secondary rehash and legacy string sweep still rely on the
  current header claim plus TG-local active markers because those paths
  destructively reuse `GCstr.gcw` chain links. The final target remains
  bitmap/dead-link sweep plus deferred string-body reclamation.

Focused regression tests for the table read cleanup:

- `tools/ci/lua_test.sh m5_tab_keylock_lookup m5_tab_next_snapshot m5_tab_forward_filter`
- Current `m5_tab_keylock_lookup` additionally checks the no-L wait counter for
  string, integer, generic, and `next()` readers.

# Table FORWARD publication

Table resize publishes `next_gen` before it has finished migrating all slots.
Old slots are then frozen to `LJ_TFORWARD`, and only after the replacement
array/hash generation contains the migrated contents does `GCtab` publish the
replacement root.

Readers must therefore treat `next_gen` as a breadcrumb, not as publication.
If a forwarded slot belongs to the table's current root generation, the reader
waits and retries from the root instead of chasing `next_gen`. Once the root no
longer points at that generation, the successor was published and the old
generation may be used to resolve stale snapshots. This remains true if the
root has already advanced again: a reader may follow the published generation
chain, but never while the generation it is reading is still the root.

This preserves Lua-visible table behavior during concurrent resize without
adding source-search tests. The invariant is covered behaviorally by
`m5_tab_forward_filter`: the fixture withholds publication of the successor root
until the successor slot is initialized, then verifies readers observe the
migrated value rather than a transient nil or the internal `FORWARD` sentinel.
It also covers the A->B->C case where a stale snapshot starts at an older
generation after a later resize has already published.

Validation:

- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/lua_test.sh m5_tab_forward_filter`

Known separate issue: `m5_tab_resize_stress` still has a pre-existing
nondeterministic traversal/GC2 crash at `886eeb3b` before this forwarding
publication change.

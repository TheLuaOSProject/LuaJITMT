# Table next generation snapshot

`lj_tab_next()` now carries the array/node generation snapshot used to decode a
hash traversal cursor into the subsequent scan. This prevents a cursor computed
with one array size from being decoded against a freshly published array size
after a concurrent resize.

The same traversal path now treats an already loaded array `FORWARD` value as an
observed handoff marker when deciding whether it may follow `TabArrayHdr.next_gen`
before the table root changes.

The change is still a bridge, not the final AHdr/NHdr iterator protocol: raw
`LJ_KEYINDEX` controls keep the existing "modified during traversal" caveat.
The important invariant is crash freedom and no exposure of internal
`FORWARD`/`KEYLOCK` sentinels while a resize publishes successor generations.

Focused guard:

- `m5_tab_next_snapshot`

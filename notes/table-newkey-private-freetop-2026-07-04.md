# Private table new-key freetop lookup

Date: 2026-07-04

`tab_newkey_private()` now uses the table hash `freetop` cursor before falling
back to the old anchor-relative full scan when inserting into a live-value
collision chain.

This is restricted to the private single-mutator window guarded by
`tab_private_mutation_allowed()`. The shared KEYLOCK/CAS path still uses its
existing scan and reservation protocol, so active MT insertion semantics are
unchanged.

The cursor is deliberately a hint, not the source of truth:

- shared insertions and abandoned claims can leave `freetop` stale;
- deleted hash entries keep their key as a tombstone and are not reusable free
  nodes;
- nil-key nodes with a non-nil value can be unpublished claims or resize
  forwarding artifacts.

For those reasons the private lookup only accepts a node when both key and value
are nil, skips the collision anchor, and falls back to the previous full scan if
the cursor cannot find a node. Tombstone anchors and nil-key/non-nil-value
anchors keep the old full-scan path so insertion does not perturb their
existing freetop behavior.

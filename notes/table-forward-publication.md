# Table FORWARD publication

Table resize publishes `next_gen` before it has finished migrating all slots.
Old slots are then frozen to `LJ_TFORWARD`, and only after the replacement
array/hash generation contains the migrated contents does `GCtab` publish the
replacement root.

Readers must therefore distinguish table-root publication from per-slot
handoff:

- If the table root has changed, the root is the stable publication point.
  Readers refresh from the current root and must not touch an old generation
  header after discovering that root change.
- Before root publication, an array reader may follow `next_gen` only after it
  acquired `LJ_TFORWARD` from the old slot. That sentinel is the per-slot
  ownership handoff marker: it says the old slot is frozen and the successor
  slot owns the value.
- Without an observed `LJ_TFORWARD`, `next_gen` is only an early breadcrumb.
  Readers keep using the current root or retry instead of chasing the successor
  before the slot has been handed off.

Hash readers do not use a per-slot successor handoff. They first check whether
their snapshot is still the published root. If it is not, they refresh from the
root without touching the old header. If it is still the root, they may use that
root-owned successor breadcrumb.

Retired node and array vectors are not physically freed while more than one TG
is live. C table scans can hold node/array snapshots across safepoints; an epoch
advance alone proves forward progress, not that every peer has dropped those
local snapshots. Reclaim therefore waits for both `LJ_TAB_RETIRE_EPOCHS` and a
single live TG before freeing retired vectors. Close-time cleanup still releases
all remaining retired vectors.

JIT table traversal (`pairs`/`next` through recorded `ITERN`/`next`) is also a
multi-TG boundary. The recorder predicts traversal result types from the table
generation it sees while recording, but the generated trace later calls
`lj_vm_next()` and consumes a traversal index that can race a peer resize. When
more than one TG is live, traversal recording is NYI and the interpreter handles
the traversal through the generation-aware runtime path. Single-threaded stock
LuaJIT traversal recording remains enabled.

This preserves Lua-visible table behavior during concurrent resize without
adding source-text tests. The invariant is covered behaviorally by
`m5_tab_forward_filter`: the fixture withholds publication of the successor root
until the successor slot is initialized, then verifies readers observe the
migrated value rather than a transient nil or the internal `FORWARD` sentinel.
It also covers the A->B->C case where a stale snapshot starts at an older
generation after a later resize has already published.

Validation:

- `make -C src -j$(getconf _NPROCESSORS_ONLN)`
- `tools/ci/lua_test.sh m5_tab_forward_filter`

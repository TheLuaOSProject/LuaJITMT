# GC2/legacy publication bridge stability

## Problem

`tests/t-jit-flush-thread-stress.lua` exposed several GC2 paranoia failures where
an object was live to the legacy collector but missing from GC2's arena bitmap.
The failures were not one root class: ownerless thread stacks, channel payloads,
worker result stacks, table raw storage, root-list objects, and interned strings
all appeared as distinct bridge holes.

The core invariant is now explicit:

- before a native storage slot is published with release visibility, any GC
  value in that slot must be published to the legacy and GC2 collectors;
- when legacy blackens an object during a real mark pass, GC2 must mirror that
  object and any raw allocation owned by the object;
- GC2 mutator barriers must not change legacy color unless the coupled
  legacy/GC2 mark bridge is active.

## Changes

- Channel sends publish payloads before the slot sequence release exposes the
  cell to receivers or GC traversal.
- Worker, receive, and join paths publish result stack slots after copying
  values into Lua stack storage.
- Ownerless threading registry states are traversed at both legacy and GC2 root
  scan edges, because they can already be marked through another path while
  their stack memory still needs bitmap marking.
- Legacy table traversal directly marks array and node storage into GC2, and
  table hash publication marks new node storage.
- Legacy propagation/finalizer/publication barriers now mirror objects into GC2
  at the blackening edge.
- Active GC2 barriers use a no-legacy-color mark path, avoiding accidental
  creation of gray legacy objects that were never queued on `g->gc.gray`.
- Legacy atomic fixpoint now mirrors the current legacy-live root spine and
  string table into GC2 before the paranoia fixpoint check.

## Follow-up

The atomic root-spine/string-table mirror is correct but broad. It should be
reduced later by making all relevant publication edges precise enough that the
atomic bridge becomes mostly a verification backstop instead of regular work.
That optimization must preserve the invariant above; replacing it with a source
conditional bypass or a lock would hide the bug instead of fixing the bridge.

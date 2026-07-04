# Arena Free-Run Binmask Fast Predicate

Date: 2026-07-03

The owner-local arena allocator now keeps `TGAlloc.binmask[kind]`, a 32-bit
summary of which free-run bins currently have a non-empty head. This does not
change allocation policy: `lj_arena_alloc()` still searches reusable free runs
before consuming the bump run, and the x64 inline allocation paths still fall
back whenever any free-run bin could satisfy the requested object.

The mask replaces repeated hot-path loads of `alloc.bins[kind][i]` with one
`test` against `lj_arena_binmask_from_ncells(ncells)`. The x64 empty `TNEW`
path still uses this as a strict predicate before consuming the bump window.
The traced one-numeric-upvalue `FNEW` path no longer does: every closure still
has ordinary identity, and its address reuse order is not a Lua-visible
semantic. That path now consumes the active bump window when it can satisfy the
fresh closure/upvalue pair, while generic arena allocation remains responsible
for free-run reuse when the bump window is exhausted.

The mask is TG-owner-local state, like the bins themselves. It is maintained
when runs are inserted, consumed exactly, split, rebuilt after sweep, cleared
for sweep/transfer, and scrubbed after corrupt-bin defensive cleanup. The
required safety property is no false negatives: if a usable bin may exist, the
fast predicate must fall back. A stale false positive is only a performance
miss and is cleared by the normal bin refresh/rebuild paths.

Focused coverage:

- `t-arena-realloc` checks exact free-run insertion, mask bit publication,
  size-threshold queries, exact consumption, and bit clearing.
- `t-x64-tnew-empty-inline` checks that the empty-table inline path sees no
  satisfying traversable run through the mask before using the bump run.
- `m6_jit_fnew_bump` checks that traced numeric FNEW allocation bypasses the C
  helper under the remaining strict inline predicates, with active marking
  still routed to the C helper for publication barriers.

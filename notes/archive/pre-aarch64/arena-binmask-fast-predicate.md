# Arena Free-Run Binmask Fast Predicate

Date: 2026-07-03

The owner-local arena allocator now keeps `TGAlloc.binmask[kind]`, a 32-bit
summary of which free-run bins currently have a non-empty head. This does not
change generic allocation policy: `lj_arena_alloc()` still searches reusable
free runs before consuming the bump run.

The mask replaces repeated hot-path loads of `alloc.bins[kind][i]` with one
`test` against `lj_arena_binmask_from_ncells(ncells)` for paths that must
preserve generic allocator reuse order. The traced one-numeric-upvalue `FNEW`
path and exact empty-table fast paths do not use the binmask as a fallback
predicate: closures and tables still have ordinary identity, but their address
reuse order is not a Lua-visible semantic. Those leaf specializations may
consume the active bump window when all publication/accounting predicates hold,
while generic arena allocation remains responsible for reusable free-run bins.
The active bump window is not published into those bins, so this does not create
overlap between the specialized allocation and later generic reuse.

The mask is TG-owner-local state, like the bins themselves. It is maintained
when runs are inserted, consumed exactly, split, rebuilt after sweep, cleared
for sweep/transfer, and scrubbed after corrupt-bin defensive cleanup. The
required safety property is no false negatives: if a usable bin may exist, the
fast predicate must fall back. A stale false positive is only a performance
miss and is cleared by the normal bin refresh/rebuild paths.

Focused coverage:

- `t-arena-realloc` checks exact free-run insertion, mask bit publication,
  size-threshold queries, exact consumption, and bit clearing.
- `t-x64-tnew-empty-inline` checks that exact empty-table C and x64 inline
  paths may use the active bump window even when a reusable traversable run is
  available for the generic allocator.
- `m6_jit_fnew_bump` checks that traced numeric FNEW allocation bypasses the C
  helper under the remaining strict inline predicates, with active marking
  still routed to the C helper for publication barriers.

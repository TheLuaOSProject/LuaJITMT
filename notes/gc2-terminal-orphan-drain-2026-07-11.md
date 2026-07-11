# GC2 terminal orphan-allocator drain (2026-07-11)

## Problem

Ordinary dead-TG reclamation preserves allocator contents by transferring all
small arenas and huge mappings to the main TG.  Huge-object tables have fixed
capacity.  If the main table could not accept an entry, the dead TG correctly
remained registered so pointer-to-owner lookup stayed valid, but shutdown had
no capacity-independent final ownership boundary.  Repeated transfer failure
could therefore leave a dead allocator, TG metadata, and OS mappings behind.

The TG list is also read by GC/JIT metadata scanners.  Its earlier private
`tg_reclaiming` writer gate did not exclude a scanner holding the shared GC2
SMR read lease, so a physical TG unlink could race the reader's `next_tg` or
TG-body load.

## Runtime transfer invariant

Huge transfer is now transactional for each entry while the dead owner is
quiescent:

1. insert or confirm the exact address, size, and flags in the destination;
2. force-tombstone the source's exact 128-bit slot, superseding an abandoned
   `BUSY`/`FREEING` state which ordinary deletion must refuse;
3. only then release-publish the destination owner id in the mapping header.

If destination insertion fills, every already-moved prefix is destination-only
and every remaining suffix is source-only.  A failed source tombstone rolls
back a newly inserted destination slot; inability to roll that slot back is a
fatal invariant violation rather than permission to return with a duplicate.
Consequently terminal hugetab destruction can claim each remaining table slot
and unmap it without dereferencing a potentially stale mapping header.  This
also removes the former capacity dependency from physical shutdown.

## Registry writer exclusion

Dead-TG reclaim is still an opportunistic, nonwaiting operation.  Its writer
order is now:

1. flush pending roots before taking an SMR writer gate;
2. CAS-publish `smr_reclaiming`;
3. require zero SMR readers and no active GC worker;
4. CAS-publish `tg_reclaiming`;
5. recheck all reader, worker, handshake, mutator, entrant, worker-count, and
   shutdown/TLS admission conditions;
6. unlink/finalize, then release `tg_reclaiming` before `smr_reclaiming`.

Any reader or worker overlap makes the pass return immediately.  It never
spins waiting for a lease holder which might itself need the allocator or VM.
TG temporary buffers are now freed and reset on their owner detach, before
`TGF_DEAD` publication, so dead TGs no longer carry that global-root exception.

## Final owner-lookup boundary

Capacity-independent unmapping cannot run immediately after GC-object
`freeall`, nor at the end of `lj_gc2_fini()`.  Trace/FFI/string/table/GC2 raw
metadata, the main and global temporary buffers, the main Lua stack, and the
lightuserdata segment vector may have been allocated by a secondary TG.  Their
later `lj_mem_free()` calls still require that dead TG's registry and allocator
metadata.

`close_state()` therefore invokes `lj_gc2_terminal_reclaim_tgs()` only after
all of those allocator-routed frees, while these objects remain live:

- `GG_State` and the embedded main TG/allocator;
- the TG registry links and writer/SMR counters;
- the embedded worker-retire list.

The helper destroys dead allocators newest-first, unlinks them, and then gives
newly unlinked unflagged worker TGs back to the retire-list storage owner.  It
fails hard unless the final registry is exactly `{ main_tg }`, `n_threads == 1`,
and the worker-retire list is empty.  No allocator-routed free or TG owner
lookup follows this point.  Main hugetab destruction happens last.  If a boot
or adoption configuration registered `GG_State` in that table, the terminal
forget primitive removes only the table entry and retains the existing final
manual GG unmap, avoiding an executing-state self-unmap.

## Storage ownership

Physical TG finalization has an atomic `LIVE -> BUSY -> DONE` state.  `BUSY`
observed by a second storage owner is fatal even in release builds; `DONE` is
the only idempotent success.  This prevents a worker/userdata path from freeing
TG storage underneath an active finalizer.

The terminal path enforces these ownership classes before unlink:

- `TGF_HEAP`: terminal-finalize, then `free()`;
- `TGF_LUA_ALLOC | TGF_DEFER_FREE`: terminal-finalize, then
  `lj_mem_freet()` through the still-linked storage owner;
- no storage flag: the TG must be present in `worker_tg_retired`; terminal
  finalization leaves storage to that list, whose subsequent ordinary fini
  observes `DONE` and performs the sole `free()`.

`TGF_HEAP | TGF_LUA_ALLOC` and `TGF_DEFER_FREE` without `TGF_LUA_ALLOC` are
fatal invalid combinations.  `threading.spawn()` allocates child TG metadata
from its already-linked parent and the child CAS-prepends when it attaches.
Thus the child occurs before its storage owner in the immutable registry order.
If the parent was reclaimed earlier, small/huge transfer rewrites that storage
owner to the tail main TG.  The terminal drain checks this dependency before
freeing a Lua-allocated TG, so a parent allocator cannot be unmapped before its
child metadata is finalized.

Custom `lua_Alloc` remains temporarily ignored as documented elsewhere.  The
Lua-allocated TG ownership check intentionally requires the current internal
arena allocator; restoring custom allocators must supply an equivalent
external lifetime handle instead of weakening this terminal proof.

## Regression coverage

- `tests/t-arena-hugetab.c` fills a tiny destination during transfer from a
  source containing abandoned `BUSY/FREEING` metadata.  It proves every address
  exists in exactly one table, physically frees the moved mapping, and then
  terminally destroys the source without a stale-header access or second
  mapping.  It also covers functional terminal forget and errno preservation.
- `tests/t-tg-registry-lease.c` holds a raw-thread SMR lease over a dead TG.
  Reclaim must return without waiting or unlinking; after lease release it
  unlinks exactly once.
- `tests/t-tg-terminal-orphan.c` forces every ordinary hugetab transfer to
  fail, denies one orphan pass with `worker_active`, places an unflagged TG on
  the real worker-retire list, and allocates a deferred Lua TG body from a dead
  parent TG.  It then runs real `lua_close()`.  On Linux a libc `munmap` linker
  wrapper retains immutable mapping identities and proves each payload and the
  parent-owned child TG body is unmapped exactly once.

Validation on the integrated default build:

- `make -j2` in `src`: passed;
- `m2_arena_hugetab`: passed;
- `m4_tg_registry_lease`: passed;
- `m4_tg_terminal_orphan`: passed.

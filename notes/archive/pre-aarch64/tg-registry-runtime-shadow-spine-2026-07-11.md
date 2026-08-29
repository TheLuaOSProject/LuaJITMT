# Runtime TG registry shadow spine

## What landed

The runtime now best-effort shadow-publishes attached `TGState` bodies through
the existing raw `gc2.tg_list` and a stable external `LJTGRegistrySlot` spine.
In the normal path each TG gets a separately allocated slot and exact
`{slot, incarnation}` key. The slot is never embedded in heap/Lua TG storage,
so existing legacy reclamation cannot invalidate a stable node address.

Slot allocation OOM deliberately falls back to the unchanged legacy attach
instead of regressing `lua_newstate`, spawn, or foreign-attach behavior. That
TG release-publishes the exact per-body `registry_shadow_missed` marker, and
the universe permanently publishes `tg_registry_incomplete` plus failure
telemetry. Future stable enumeration or reclaim authority must reject an
incomplete spine. A later duplicate attach may successfully fill this body's
slot and clears its per-body marker before head publication, but it cannot
clear the sticky universe-wide incompleteness veto.

Runtime slots are deliberately single-incarnation in this slice. A new slot is
claimed as ATTACHING, its tagged body is published, and its `next_all` link is
CAS-prepended to `gc2.tg_registry_head`. `next_all` is immutable after that
successful head CAS. EMPTY slots remain linked but are not reused; all nodes
are freed only after the late terminal legacy TG/orphan drain at universe
shutdown.

## Attach transaction

For a new TG, `lj_tg_attach()` performs this shadow transaction:

1. Require the dormant per-TG root descriptor to be IDLE.
2. Claim ATTACHING with its owner lease, exact-publish the tagged TG body, and
   link the stable slot. On slot OOM, mark the body/universe incomplete and
   continue through the legacy-only path.
3. Run the existing phase/handshake catch-up without changing its root tuple.
4. CAS-publish the legacy TG list and retain its existing trace-boundary/root
   flush behavior.
5. Mirror ATTACHING to LIVE.

The slot is linked before catch-up, but it may already expose the same root
fields the legacy attach path published before entry. That is intentional:
stable enumeration is disabled, ATTACHING is not root authority, and hiding
then restoring those roots without an exact active descriptor can miss an
entire preempted GC cycle. This slice therefore preserves the legacy attach
behavior instead of pretending a bounded phase rescan proves coverage.

LIVE is only a shadow lifecycle state today and does not claim root
completeness. The legacy pre-attach owner gap also remains: spawned/foreign
`lua_State` ownership and raw `tg_hint`/TLS setup can precede raw-list
insertion, so legacy `lj_tg_find_owner()` is not repaired by the shadow
transaction. Exact root hiding, ATTACHING enumeration, and activation
revalidation must land together with descriptor admission; a future major
request is not a repair for a missed current cycle.

## Detach and physical reclaim

Detach exact-transitions LIVE to DETACHING before clearing descriptor roots,
state hints, or the current raw TLS binding. The primary threading-worker
cleanup explicitly starts DETACHING before its earlier state-release boundary.
After the existing terminal flushes and root clears, DEAD is published and the
slot transitions to RETIRED. RETIRED closes stable borrow admission but does
not authorize body destruction.

The old TG-list writer remains mandatory positive authority. Only after all of
its existing thread-count, handshake, worker, SMR, owner, SSB, and raw-storage
predicates hold does it attempt the additional exact
`RETIRED/1 -> RECLAIMING/0` edge, before mutating or transferring allocator
state. An outstanding stable borrow makes that opportunistic pass retain the
TG. A failed allocator transfer keeps the raw node in non-borrowable
RECLAIMING for a later legacy retry. Once admitted, any finalization/free owned
by that legacy reclaimer runs while the tagged pointer is non-borrowable, and
the stable body is exact-cleared before EMPTY is exposed.
Unflagged worker TGs and ordinary Lua-allocated TGs instead have a separate
worker-retired/userdata raw storage owner after raw-list unlink. Their slot is
cleared after stable admission is closed and borrows reach zero, while that
legacy owner retains the now-unregistered body until its later finalization.
Thus a stable slot never retains a borrowable pointer to freed TG storage,
while the new token is never sole reclaim authority.

After raw main TLS is cleared, the embedded main TG is mirrored through
DETACHING, RETIRED, and RECLAIMING before its SSB, temporary buffer, hugetab,
allocator, or other subordinate storage is destroyed. The tagged body remains
closed but present through the final raw-owner work, then becomes EMPTY at the
final `close_state()` boundary. Slot storage is freed only after
`lj_gc2_terminal_reclaim_tgs()` has removed late allocator orphans which still
need their stable keys.

## Focused coverage

`tests/t-tg-registry-lease.c` now checks both publications, main and secondary
LIVE state, tagged body identity, immutable-spine membership, DETACHING/RETIRED
lease preservation, a stable borrower independently vetoing an otherwise
admissible legacy reclaim pass, and exact body clear to EMPTY while the node
remains linked. It also injects portable one-shot slot OOM to cover both a
fully legacy missed attach/reclaim and missed-then-idempotent-retry publication,
including sticky universe telemetry. Finally, it proves that an admitted main
borrow vetoes terminal prepare before subordinate destruction and that release
allows the idempotent RECLAIMING prepare to complete.

## Temporary constraints and follow-up

This is intentionally a shadow/negative-veto migration slice, not the final
nonblocking registry:

- each normal attach uses system `malloc`; slot OOM preserves legacy behavior
  but makes stable authority unavailable for the rest of that universe;
- failure of constant-state claim/tag operations after a successful allocation
  remains a corruption-only fail-stop, not an OOM path;
- runtime slots are not reused, despite the primitive supporting incarnation
  reuse;
- raw TLS still caches `TGState *` and holds no long-lived stable borrow;
- owner lookup, memory-owner lookup, safepoints, profiling, strings, GC, and
  other enumeration still walk the raw list and can return escaping raw TG
  pointers;
- stable iteration must eventually preserve distinct NOT_FOUND, BUSY,
  PINNED, and ATTACHING-gap outcomes, and raw-return APIs need borrow-carrying
  replacements before conversion;
- the eventual stable ATTACHING scanner will need a nonblocking replacement
  for `lj_gc2_scan_cycle_owner_tg_roots()`'s legacy blocking SMR reader path;
- stable PINNED-body enumeration and descriptor helping are not integrated;
- raw list/SMR and every existing physical-free veto remain required until TLS
  and all raw holders migrate.

The next runtime slice should install a stable TLS tuple plus one long-lived
borrow, then convert individual enumerators to scoped borrows without changing
GC close authority. Slot pooling/reuse and a recoverable complete-spine
allocation strategy can follow only after that lifetime path is complete.

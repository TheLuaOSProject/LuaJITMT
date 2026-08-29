# GC2 dormant HugeTab table-token enumerator (2026-07-18)

## Status and scope

This checkpoint extends the test-helper-only table-token prototype from small
arenas to physical HugeTab allocations. It is intentionally still dormant:

- no production mutator publishes table work through this request helper;
- no production worker or phase driver invokes either token enumerator;
- no close predicate interprets the sticky requested generation as work or as
  a completed pass; and
- legacy table `NEEDSCAN`, grey/SSB/recovery publication, and close gates stay
  authoritative.

The Huge lane adds a resumable `{stable TG slot, incarnation, physical
HugeTab slot}` cursor: one pointer, one `uint64_t`, and two `uint32_t` fields,
or 24 bytes on the supported 64-bit targets. Together with the preceding
small-lane fields and private deferred-work epoch, the dormant per-global cost
is 104 bytes before any surrounding compiler padding. This changes only the
fork-private `global_State` layout, not the public LuaJIT API or ABI.

## Stable TG and mapping lifetime

The scanner enumerates only the stable TG registry spine. It never falls back
to the legacy raw TG list and never scans `g->main_tg` separately, so main has
one identity rather than a duplicate special case. Each budget unit consumes
exactly one stable node or one physical HugeTab slot. The cursor advances
before candidate work, resets the physical slot on incarnation change, and
therefore cannot be monopolized by one BUSY, FREEING, saturated, or otherwise
transient slot.

The outer lifetime order is:

1. `worker_active` serializes an admitted MARK, WEAK, or SWEEP scanner turn;
2. a FULL OPEN-SMR or counted SWEEP_STABLE outer registry reader prevents the
   legacy TG metadata writer from reclaiming the observed raw TG;
3. a split-validated stable slot snapshot proves one exact tagged body and
   incarnation;
4. immutable `tg->gl` and reverse `{slot, incarnation}` identity are checked
   before flags or HugeTab are read; and
5. a direct physical-slot token lease retains the mapping before its embedded
   stamp, GC header, or payload can be inspected.

The scanner accepts canonical ATTACHING, LIVE, DETACHING, RETIRED, and—only
under the scanner's worker/outer exclusion—RECLAIMING identities. RETIRED and
RECLAIMING require `TGF_DEAD`; RECLAIMING additionally has the registry
protocol's exact zero-lease shape. A FULL request reader may observe ordinary
states but never admits RECLAIMING. EMPTY/EXHAUSTED null bodies are skipped;
the canonical ATTACHING body-publication gap is transient. PINNED, malformed
components, wrong body tags, `gl`/reverse-key contradictions, and impossible
closed-body flags pin reclamation fail-closed. No TG pointer survives release
of the outer reader.

`tg_registry_incomplete` is never a false end-of-pass acknowledgement. The
request rejects before outer admission and rechecks at its body handoff so it
cannot manufacture work outside the stable universe. Enumeration still makes
progress on already-known stable nodes, while recording the incomplete
universe as transient.

## Exact HugeTab candidate proof

The new arena helper exposes the fixed embedded table stamp only while an
exact physical-slot token lease is active and the pointer names the one lawful
huge-body geometry, `arena + sizeof(GCAhdr)`. It grants no payload authority.

For LIVE in MARK or WEAK, the scanner transfers that same counted token lease
directly into an ordinary HugeTab reader, without an address lookup or reader
count gap. Under that retained reader it validates READY/TRAVERSABLE flags,
size, exact base, table type, and covered header range; performs the existing
exact dirty/cycle table proof; and completes only the captured
`PENDING(D) -> NONE(D)` generation. A refresh or proof race retains the newer
token for a later turn. LIVE in SWEEP remains PENDING and no body byte is read.

DEFER_FREE is an irreversible logical-death boundary. Its scanner arm reads
only allocator header state and the embedded token, never the GC header or
table payload. Exact terminal completion is deliberately phase-independent in
MARK, WEAK, and SWEEP: restricting it to SWEEP would leave a token-gated dead
mapping unable to close during a later MARK/WEAK turn. The final physical-slot
lease release performs the ordinary DEFER_FREE-to-FREEING handoff. FREEING and
BUSY grant no stamp authority and are skipped; reader-count overflow is a
fail-closed no-reclaim condition.

The sticky requested-generation maximum remains wake and diagnostic advice
only. Explicit scanner turns no longer use `requested == 0` as an entry gate:
a publisher may stop after installing durable PENDING and before raising the
hint, and that schedule must remain helpable. Completion changes only the
exact token and diagnostic counters; it does not clear the sticky hint or
pretend a topology pass is complete.

## Request and boundedness limits

The dormant request first tries the small allocation identity, then searches
the stable TG spine for an exact HugeTab reader. It requires a FULL outer
reader, exact reverse identity, lawful huge-body geometry, a retained table
body, and one stable MARK/WEAK cycle before transferring the requested
generation. It has no raw-list, address-only, or main-TG fallback.

The resumable Huge scanner is identity-budgeted and lock-free. `budget` bounds
stable nodes/physical slots attempted, not payload work: an admitted exact
table proof is necessarily O(table size). Two convenience paths remain
test-only and must be bounded or replaced before any production reuse: the
request's full stable-spine owner search and `scan_one`'s exact-target
stable-spine/physical-slot search. Neither is used by the resumable production
candidate design.

## Deterministic evidence

The focused fixtures cover:

- lawful LIVE and DEFERRED physical token leases, direct reader transfer,
  malformed copied-lease geometry rejection, and FREEING/BUSY no-authority
  results;
- stable Huge request success plus rejection while the stable universe is
  marked incomplete;
- exact MARK and WEAK table proof with payload accounting;
- a physical cursor placed on one exact TG incarnation/slot completing in one
  budget unit, with no duplicate main fallback;
- manually installed PENDING work completing while the advisory requested
  hint is zero;
- SWEEP LIVE retaining PENDING without reading a poisoned GC type byte;
- MARK, WEAK, and SWEEP DEFER terminal completion with a poisoned body and no
  payload-counter change; and
- the dead-owner transfer cycle: first reclaim closes the stable slot at exact
  RECLAIMING/zero leases but cannot move a pending-token mapping, the bounded
  scanner completes it through the tagged closed body, and the next ordinary
  reclaim transfers the mapping to main and clears the slot.

Strict helper builds and the focused traversal test pass. Independent repeat
coverage completed 30/30 runs of the final RECLAIMING fixture and a clean
Valgrind run. The standalone arena fixture passes GCC `-Wall -Wextra -Werror`,
repeat runs, and Clang ASan/UBSan.

## Remaining production cutover work

1. Define topology/pass epochs and an exact no-work acknowledgement spanning
   stable TG attach/detach and HugeTab insert/delete/transfer churn. Neither a
   wrapped cursor nor the sticky hint is an end-of-pass proof.
2. Integrate token publication, bounded scanner scheduling, and token/topology
   close vetoes into MARK/WEAK/SWEEP drivers before any legacy membership is
   removed.
3. Bound or eliminate the test-only full-spine request/target convenience
   searches, and choose production quanta around O(table-size) exact proofs.
4. Complete generation, dirty-epoch, cycle, and topology wrap protocols and
   the remaining capacity-full weak-overflow allocation-failure matrix.
5. Cut over terminal unmap, diagnostics, and every producer/consumer only
   after mixed small/Huge churn and phase-close stress prove no hidden legacy
   dependency.

Custom `lua_Alloc` remains temporarily outside this b1.2-era GC2 tranche as
documented elsewhere. The dormant helper rejects it rather than publishing
work that the arena-backed physical enumerator cannot consume.

# Persistent table-authority overflow: independent design audit

2026-09-05. Read-only production review; no runtime prototype, production edit,
runtime test, or new timing is claimed here. The source identities are in
source-manifest.json. Root owns integration. This audit follows the optional
sidecar section of notes/gc-table-authority-wide-prototype-2026-09-05.md.

## Decision

A persistent overflow domain is a sound way to keep the ordinary CAS64 path.
The narrow complete comparison candidate is **storage reserved before arena or
table publication**, with unchanged 16-byte inline stamp/token entries and a
wide authority that is monotone for the whole mapping/cell lifetime. Invalidate
that wide authority **before** publishing the permanent inline sentinel.
That ordering avoids exposing stale wide coverage from an earlier cell
incarnation and leaves the current allocation assembly resets valid.

A purely lazy allocation inside the post-store overflow barrier has no complete
failure protocol in the present collector. It must not be accepted as the
32-bit-cliff repair merely because normal successful allocation works.
Permanent NO_RECLAIM is a safe last-resort containment state, but cannot later
be cleared and does not establish continued collection. A new recovery-only
authority mode would need a separate proof and implementation scope.

The original wide-AoS timing is not design-selection evidence: its independent
emitted-code audit found hard-coded SHL 4 indexing after the C entry became
32 bytes. The note now retracts the comparison. The sidecar proposed here
retains sizeof(LJGC2TabStamp)==16 and token offset 8; all existing stride-16
paths remain geometrically correct. Fresh measurements are still required.

## Existing authority and lifetime boundaries

- src/lj_gc2.c:15535–15603 supplies the inline stamp lookup, dirty snapshot,
  proof publication, and post-store invalidation. The current proof identity is
  dirty32; covered_cycle32 shares its CAS64. A dirty-MAX bump clears coverage
  and enters absorbing global NO_RECLAIM.
- gc2_table_scan_current and gc2_table_scan_coalescible at 18417–18440 are
  different predicates. The former participates in graph discovery and legacy
  rescan dedupe; the latter excludes saturation when consuming suppressible
  SSB requests. Replacing only the latter cannot implement a new authority mode.
- gc2_traverse_tab_rec at 18865–19078 captures authority before child loads and
  publishes proof before legacy NEEDSCAN/count settlement. Its post-clear
  check repairs a racing writer. Exact-token traversal publishes proof, then a
  separate ticket owner completes the exact token. Both modes need the new
  discriminated proof snapshot; neither may adopt a new domain after scanning.
- Small stamp storage is allocated before traversable arena publication.
  Mapping admission alone permits persistent side-token access; exact readable
  allocation admission is additionally required for dirty/proof operations.
  Small FREE token completion at 19214–19225 intentionally reads no body.
- Huge DEFER_FREE completion at 19423–19434 owns only a retained mapping header.
  It completes the exact token without a GC header or proof read. A live scan
  must first transfer the header lease into LJHugeReader and retain the body.
- Exact token generations survive small-cell reuse. Nothing in this proposal
  changes token encoding, generation, descriptor transfer, completion tickets,
  phase eligibility, REDIRTY ownership, or terminal token vetoes.

Line numbers refer to the recorded source snapshot and may move on integration.
Those contracts must remain explicit: an extension pointer, header-only lease,
or old token address never supplies table-body authority by itself.

## A concrete reuse/promotion counterexample

The note's proposed per-incarnation reset is sound only if *every* new table
incarnation resets its already-existing wide entry before publication.
arena_gc2_prepare_incarnation is not the only allocation reset:

1. VM empty TNEW computes cell*16, checks token state, and writes only inline
   state=0 directly in src/vm_x64.dasc:4537–4542,4650–4656,4722.
2. Native FNEW likewise resets inline words in src/lj_asm_x86.h:1568–1569.
   Its stamp address calculations at 1582 and 1643 also hard-code stride 16.
   These functions/upvalues can occupy cells that previously held tables.

If a patch resets wide state only in arena.c, this legal compressed schedule
exposes a false proof:

1. An old table at cell X leaves wide entry W with coverage for current cycle C,
   then dies; all old body owners and the exact token are gone.
2. VM TNEW reuses X and resets inline state, but W still covers the old body.
3. The new table receives child B. Its ordinary inline dirty bump and queue
   publication finish. B's completed request has not yet been consumed.
4. A later writer reaches the promotion boundary, switches inline to sentinel,
   and pauses before the proposed wide bump.
5. B's queue consumer dispatches through the sentinel, reads old W.coverage=C,
   and can suppress B's completed request despite never scanning the new body.

The missing proof concerns an already-completed request, not just the paused
writer's own unfinished store. Body retention pins the parent allocation; it
does not manufacture a traversal of its children. Other global gates may
conservatively retain a particular schedule, but are not authority for this
new clean-stamp shortcut.

Covering every C/VM/JIT reset or forcing an early C fallback could repair that
version. A simpler alternative is to **never reset W on cell reuse** and always
invalidate W before making it authoritative. The inline reset remains private,
unchanged, and sufficient to return the new allocation to ordinary CAS64 mode.

The accompanying interleaving-model.py records this logical counterexample and
the ordering repair. It is a small state model, not a runtime/lifetime test.

## Proposed state machine

Keep inline I={covered_cycle32,dirty32}, the exact token word, and their current
layout. Reserve dirty UINT32_MAX as WIDE; the maximum ordinary dirty value is
UINT32_MAX-1. A WIDE inline word has cycle zero and is permanent for that
allocation incarnation. It never carries a wide cycle or counter fragment.

Each small cell's persistent W is one aligned atomic
{era64,covered_cycle32,dirty32}. W is zero-initialized once, before its containing
ready extension is published, and is never reset while that mapping exists.
Huge uses one corresponding W for its mapping. Thus low-serial wrap renews era
monotonically; cell reuse does not create a second reset protocol.

Ordinary post-store invalidation, while exact table/body admission remains held:

    I := acquire inline
    if I is ordinary and dirty < UINT32_MAX-1:
        CAS64 I -> {cycle=0, dirty=dirty+1}; retry on contention
        return
    locate already-initialized persistent W
    CAS128 W -> next nonwrapping {era, dirty, cycle=0}
    if I is ordinary at UINT32_MAX-1:
        CAS64 current I -> canonical WIDE
        retry only that mode CAS if an inline scanner changed coverage
        a peer's already-published WIDE is successful domain handoff
    publish the existing semantic queue/token/recovery request

Every writer performs its own W invalidation before its own request publication.
A losing mode installer does not reset W, overwrite a peer's state, or skip its
invalidation. One successful wide invalidation is sufficient for that writer:
if its inline mode CAS loses to a peer's WIDE, the shared domain is already
active. Subsequent W mutations are monotone and cannot restore older coverage.
Unexpected inline regression under the retained body scope is malformed
lifetime authority, not a new initialization opportunity.

The extension pointer is release-published before W access and before the
sentinel CAS. Private candidates are completely initialized before publication;
there is no shared INIT owner. A failed pointer-CAS candidate was never visible
and can be disposed by its own thread. Published storage is never replaced.
A suspended initializer or mode installer cannot prevent another admitted
writer from using/publishing a complete extension and switching to WIDE.

This describes a generic ready-pointer protocol. In the recommended fully
reserved implementation the pointer is already present before mapping
publication, so no allocator, private initialization, or pointer contention
exists in the post-store barrier at all.

Scanner and coalescer requirements:

- Capture an explicit INLINE or WIDE snapshot before reading child slots.
  An INLINE snapshot may publish only to ordinary inline state with the same
  dirty serial. If its CAS loses to WIDE, it must rescan; it cannot reload W
  and convert the completed old scan into a wide proof.
- A WIDE snapshot compares both era and serial in the full CAS128. Its retained
  exact body scope prevents cell reuse, so mode cannot revert underneath it.
  Rechecking inline may diagnose corruption, but cannot replace body admission.
- Both current-scan and semantic-coalescing predicates dispatch by mode. Inline
  sentinel never means dirty saturation with valid inline coverage. Missing W
  behind a sentinel is corruption, not an absent proof or permission to
  initialize storage.
- Wide coverage is zero before the first observer can acquire WIDE. A scan
  started after promotion can safely cover completed earlier writes; the
  wide invalidation's release and the mode publication order those stores.
- A writer that pauses after invalidating W but before the sentinel does not
  own an exclusion state. Other writers can advance W and complete promotion.
  On resumption it cannot erase their proof with an initialization store.
- Keep proof-before-token-completion, legacy post-clear repair, unsuppressible
  denied-admission recovery, and the existing phase/worker authority unchanged.
  A delayed MARK/IDLE publisher invalidates whichever mode exists when it acts.
- At full era-plus-serial exhaustion, reject all wide proof/coalescing authority,
  preserve concrete work, and retain the existing sticky containment behavior.
  This finite boundary and independent GC-cycle/token namespaces remain open.

Mode promotion changes no weak/FINREG policy. Traversals still record weak work,
handle finalizer claims as RETRY, and use private discovery instead of semantic
dirty increments. In particular, do not make every WIDE table permanently
uncurrent: a mutually recursive pair could continually rediscover each other.

## Allocation failure after an already-committed store

These tempting shortcuts are not complete:

| Shortcut | Counterexample or missing obligation |
| --- | --- |
| OOM: leave the last ordinary proof untouched and enqueue normally | A paused old inline scanner publishes that unchanged serial after the store; the SSB coalescer consumes the new request. |
| Publish sentinel, then allocate/initialize | A reader sees sentinel with no lifetime-valid W; another reader can use W before late initialization overwrites its update. |
| Pin NO_RECLAIM, allocate later, clear the veto | NO_RECLAIM also represents unrelated unclassified edges and is absorbing for the universe. A sidecar success cannot discharge it. |
| Reserve one shared INIT bit and have peers wait | A suspended initializer owns progress; this defeats the nonblocking purpose. |
| Leave durable recovery pending until allocation succeeds | Safe retention can be arranged, but persistent OOM can prevent the collection that would free memory. It proves conditional retry, not continued collection under allocation failure. |
| Disable all scan proof forever and let recovery drain normally | The current table traversal requires proof to settle/requeue; permanently failing it can retain/requeue the same request forever. Private cyclic discovery also uses scan_current. |
| Start preallocation far below the boundary | Extra serial headroom reduces frequency, but an arbitrarily paused allocator or persistent OOM can exhaust that finite reserve. |

A recovery-only fallback may be possible, but is not a one-branch repair.
It would need a reserved inline mode that cannot be confused with a ready-W
sentinel; every semantic publisher would have to choose an unsuppressible
obligation; traversal would need to distinguish a discovery visit certificate
from a proof that can consume a post-store request; and recovery CLAIMED/REDIRTY
and exact-token completion would need one common closure proof. The current
rescan_later_force still contains a current-scan shortcut after a failed
reservation, so merely switching one caller to force is insufficient.

The direct child barriers do not eliminate this requirement: public semantic
root/table publication can follow arbitrary raw payload writes and does not
carry all the changed child values. The g-only void ABI cannot retroactively
reject or roll back the already-committed write on malloc failure.

Therefore reserve sufficient storage before such writes can ever occur:

1. Simplest candidate: reserve/zero the dense wide array as part of traversable
   arena creation; reserve the one huge W before huge mapping publication.
   Allocation failure unwinds unpublished storage and follows the existing
   allocation-failure path. A later overflow barrier cannot newly fail for W.
2. Lower-memory alternative: reserve sparse records or indexed chunks before
   the first table at a relevant cell/group is published. This requires C
   construction plus VM TNEW and native allocation-path participation.
   A missing reservation must cause fallback before cursor/body/publication
   effects, with a C path that can report OOM while the object is private.
3. A lazy-only prototype could explicitly retain the current permanent global
   veto on OOM as containment, but must remain an incomplete progress prototype
   and cannot be presented as the release-cliff fix.

Pre-reserving address space with demand-zero OS pages may reduce observed RSS,
but is a separate measured allocator choice. It does not make page faults,
calloc, kernel allocation, or OS out-of-memory handling wait-free. MAP_NORESERVE
does not supply a stronger failure guarantee than ordinary successful runtime
allocation. Do not call nominal byte counts below measured committed memory.

## Storage choices and cost

Baseline: 4096 entries * 16 bytes = 65536-byte sidecar, plus 65536-byte arena.
The following are nominal metadata payloads, excluding malloc headers, padding,
mapping granularity, temporary losing candidates, and peak allocator effects.

| Choice | Additional small metadata | Lookup after promotion | Notes |
| --- | ---: | --- | --- |
| Dense W[4096] | 65536 bytes per arena needing/reserving it | One persistent root and direct cell index | Simplest publication and terminal free; eagerly reserved total arena+sidecars is 192 KiB, 50% above baseline, while preserving the current GCAhdr geometry if an audited union is used. |
| Dense W for allocation-capable cells only | 55680 bytes with first cell 616 | Direct cell-minus-first index | Avoids 616 unusable entries; geometry must derive from compiled LJ_AFIRST_CELL. |
| Append-only sparse cell records | At least 32*P bytes for P ever-promoted cells | O(P) list search per promoted access | Record can contain 16-byte W, next pointer, cell index/padding. CAS head publication must rescan after failure to prevent duplicate records. Worst case and allocator overhead can exceed dense storage. |
| Sparse records plus per-cell pointer index | 32768 bytes + record storage once directory exists | Constant-time two-pointer lookup | Low occupancy saves versus dense, but a 4096-pointer directory is already 32 KiB; approximately 1024 nominal 32-byte records consume the remainder of the dense budget. |
| 64 indexed groups of 64 W entries | 512-byte directory + 1024 bytes per populated group | Constant-time directory/group lookup | Intermediate option; no linked search, fully ready group publication, no shared INIT. Reserving a group before table construction moves OOM before writes, but adds construction-path work. |

The append-only list has bounded address geometry but not an attractive hot
overflow lookup: a table that has crossed 2^32 updates may continue to receive
many writes, and a growing unrelated prefix makes each such write expensive.
Its head CAS can starve a particular installer under contention while peers
progress. Do not describe that algorithm as wait-free.

Normal non-promoted access needs no extra pointer lookup or CX16; it retains a
CAS64 and a threshold branch. Promoted access pays pointer lookup and full
CAS128 snapshot/update, plus the same token/publication costs as before.
The monotone-W variant imposes no wide reset on ordinary VM TNEW/FNEW reuse.
Never replace CX16 snapshots with mixed-width loads without a separate proof.

Dense reservation increases memory held by empty reclaimed spares until their
terminal unmap; adoption must not discard W to recover that memory. Capacity/
retained-memory controls from the arena work therefore matter for this choice.
The revised prototype must measure construction/insertion and retained bytes
in addition to barrier throughput; common-path CAS width alone is not a speed
claim.

## Existing-header reuse: plausible only as explicit tagged layout

Do not borrow grey, retire_obj, remote_free, next, descriptor, progress, lifecycle,
or token fields. They carry independent live/terminal authority.

Two physically present kind-exclusive regions merit a small dedicated layout
audit instead of casually casting existing fields:

- Non-huge arenas never use the embedded huge_tabstamp as their cell token.
  An explicit union could give this 16-byte region a small-overflow-root member.
- Huge mappings have no small cell sidecar and currently leave gc2_tabstamp
  null. An explicit union could give that pointer-sized region a huge-W member.

The discriminator is the immutable huge mapping kind, not transient ownership,
phase, EMPTY_RECLAIMED, or allocation gct. Accessors must select the correct
union arm before interpretation. This could retain GCAhdr size 128, existing
offsets, and first usable cell 616. Treat those numbers as conditions to verify
with compile-time and emitted-code checks, not permission to reuse bytes now.

Current small terminal unmap frees gc2_tabstamp at arena_unmap_claimed.
Both huge unmap functions directly unmap their mapping and free no sidecar.
Any heap W requires explicit destruction in both huge terminal paths and
unpublished-allocation unwind, as well as small extension destruction. Generic
free(lj_arena_gc2_tabstamp_acq(a)) must not free a union arm with the wrong
destructor/layout. Plain mappings must never acquire table overflow metadata.

An appended root pointer in LJGC2TabStampArena is less invasive for small
storage; it preserves the cell array base/stride but grows that allocation.
It does not solve huge storage. A new dedicated GCAhdr field is conceptually
clearer but can change header padding and every arena bitmap offset. Either
choice needs a full rebuild and explicit generated allocation checks.

## Mapping, late publication, and reclamation

- The ready pointer and all published records/groups are append-only until
  terminal mapping retirement. They remain reachable across owner transfer,
  empty-arena adoption, free-list rebuild, and ordinary collection cycles.
  A private losing candidate is the only immediately disposable storage.
- A small pointer publisher/reader retains the shared registry mapping and
  exact allocation scope. Terminal unmap closes fresh remote admissions and
  requires old counts plus recovery/root/lifetime/dtor/token/descriptor owners
  clear. Do not free the extension at token NONE, cell FREE, or an empty-spare
  certificate: other cells or header readers can still own the mapping.
- Huge W stays with the mapping, not the TG-embedded HugeTab wrapper, whose
  address may retire or transfer. Exact slot/body readers pin the mapping;
  count-zero/admission-close removal owns final destruction.
- Header-only small FREE and huge DEFER_FREE token completion continues touching
  only the existing token. It must not locate, reset, allocate, or inspect W,
  even if the old inline word says WIDE. Completing a dead token never needs a
  graph certificate.
- Late free wins according to the existing exact lifetime/DEFER_FREE protocol.
  Admission refusal publishes unresolved work or leaves the retire-owner
  obligation; it must not create/refresh W through an unadmitted body pointer.
  A retained delayed installer blocks that cell/mapping's physical reuse until
  it leaves; it cannot resurrect a terminal body through the sidecar.
- Monotone W need not itself pin a free cell or arena. Once all real readers and
  requests are gone, ordinary private reuse resets inline state and retains W;
  eventual terminal unmap can destroy W. A fresh mapping has a fresh domain
  because no prior mapping reader may survive that destruction.

## Minimum independent controls before authorizing production

1. Real old INLINE scan paused before proof, actual raw descendant store and
   boundary promotion, then old scan release. Require new descendants marked,
   a new scan, drained work, and no reclaim veto for both small and huge.
2. Two installers for one cell and different cells: pause before ready-pointer
   publication, after pointer publication, after W invalidation, and before
   mode CAS. The other actor must complete; losing candidates are freed once;
   installed W is never reset. Old INLINE CAS loses to WIDE.
3. Reuse a cell with deliberately current-cycle old W coverage through the
   actual VM TNEW path, then promote while paused. Require no old proof or token
   corruption. Exercise FNEW pair cells and untouched adjacent entries too;
   inspect generated stamp stride, token offset, and full reset width.
4. Header-only small FREE and huge DEFER_FREE completion with inaccessible body
   pages and W deliberately inaccessible/unneeded. Token completion must not
   acquire a body or dereference/reset W.
5. Hold old body readers across free/reuse and huge transfer/unmap attempts.
   Extension stays allocated; new incarnation/terminal release only after real
   authority clears. Late publications and losing destructive claims preserve
   their existing obligations.
6. Fail each private reservation allocation before mapping/table publication.
   No partial public object, extension leak, sticky runtime veto, or dropped
   payload store. Once a table is published, inject general malloc failure at
   promotion and verify that the reserved path makes no allocation.
7. If lazy-only recovery is pursued, force persistent allocation failure with a
   completed raw write, old scanner, cyclic graph, weak values, and finalizer
   claims. Require an explicit convergence/result contract; eventual successful
   allocation alone is not its negative-control answer.
8. MARK/WEAK/SWEEP crossings, old hint-clear/COUNTED races, exact-token replace/
   complete races, full wide terminal saturation, low-serial ABA, cycle and
   token finite-boundary controls. Keep private discovery free of semantic
   dirty increments.
9. Strict/assert plus ASan/leak checks and normal stock/focused suites, followed
   by fresh normal before/after timings with identical safety fixes. Measure
   normal writes/insertion, forced promoted writes, initialization cost,
   retained metadata/RSS, and terminal free. No current speedup claim.

Recommended next authorization is an isolated reserved-dense-W comparison
prototype with this monotone/pre-invalidation protocol, not production
integration. Its known cost is memory; its normal-path cost and actual retained
memory still need measurement. A lower-memory constructor-reserved indexed
chunk design is the next candidate if dense memory is unacceptable.


Durable evidence: `notes/evidence/gc-table-overflow-reservation-audit-2026-09-05/`. Original executable artifacts
remain at the `/tmp` paths in the records.

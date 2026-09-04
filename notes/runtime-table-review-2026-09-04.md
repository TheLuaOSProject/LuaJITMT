# Runtime table resize review

Date: 2026-09-04

This review inspected the current `src/lj_tab.c`, `src/lj_tab.h`,
`src/lj_obj.h`, the relevant GC2 table traversal/weak clearing code, the table
plan/model, and `notes/table-resize-descriptor-2026-07-29.md`. This source review
is separate from the executable validation of the installation fix, landed as
`4a46db9e` and recorded in `notes/table-descriptor-install-pair-2026-09-04.md`.
References are to the reviewed working tree;
symbol names are the stable locators if concurrent edits move line numbers.

Production resize is still owner-driven. The persistent descriptor remains
metadata-only, and neither its phase names nor its current fixture proves a
helpable production migration. The candidate protocol later in this note is
**unmodeled and unproven**, not an implementation decision or a completion
claim.

## Verified installation defect and current fix

The previous `lj_tab_resize_desc_install()` separately synchronized
`weak_record`'s capacity shadow and CASed stable table control to descriptor
control. A failed control CAS did not undo the shadow write. Checking the
stable control word before that write did not protect the write itself.

A harmful interleaving is:

1. Installer A samples stable control/capacity and passes its equality check,
   then pauses before writing the capacity shadow.
2. Installer B installs and reaches capacity cutover. Alternatively, a real
   ordinary resize grows the vector and B then installs over that generation.
3. A resumes, overwrites the shadow with A's old capacity, and loses its control
   CAS. A's cancellation cannot repair B's shadow.
4. B's snapshot sees the wrong capacity, or `tab_resize_desc_detach_control()`
   publishes the corrupted shadow as stable capacity.

No table-control ABA is required for the shadow overwrite. A repeated stable
word can create additional stale-snapshot cases, since stable control contains
only `{acap, owner}`, not a structural generation identifier.

The fix at `src/lj_tab.c:2223` uses
`lj_tab_struct_weak_pair_cas()` to publish descriptor control and its capacity
shadow in one `cmpxchg16b`, preserving the sampled semantic weak cycle/state.
A concurrent weak-state update cancels this bounded installation attempt and
leaves both current words intact. This matches the existing paired cutover
protection in `tab_resize_desc_prepare_clearing()`.

The regression work reproduces the capacity loss with an actual
`lj_tab_resize()` followed by a competing descriptor installation. The current
`exercise_install_competing_generation()` fixture uses that schedule;
`exercise_install_weak_update()` separately checks cancellation on a semantic
weak-state race. The original pre-control hook followed the independent shadow
store, so exposing the overwrite requires pausing before that store in the old
implementation. The new paired operation has no such gap. The installation
note records executable failure/pass evidence; this note records the source
review and fixture design, not an independently rerun test result.

## Constraints on the next production change

### Allocate and retain the whole attempt before owning the table

A private descriptor plus successor/resource bundle before installation is
preferable to installing a descriptor and then allocating behind it. A paused
allocator must not hold the table's structural exclusion. The bundle needs
target array/hash sizes, both successor roots, migration metadata, completion
storage, and cancellation/free ownership before the irreversible migration
boundary. Use bounded failure cleanup before throwing; partial allocation must
not leave raw storage owned only by a vanished C frame.

Preallocation must carry its exact source root/sizing assumptions. The current
installed snapshot is captured *after* the control CAS
(`tab_resize_desc_capture_snapshot()`, `src/lj_tab.c:1769`). A prepared bundle
cannot simply accept that later snapshot: validate the exact array/node
identities, visible sizes, capacities, and retirement state against the facts
used for sizing. An equal `{acap, owner}` word does not prove the old vector
generation is unchanged.

Sizing must cover late inserts and the entire array tail that can move to the
hash, not just a pre-install live-value count. The existing resize accounts
for late hash inserts at `src/lj_tab.c:3147` and counts array tail capacity in
`tab_rehash_hashcount()`. Destination exhaustion must be impossible after the
irreversible boundary, or there must be a complete helpable recovery protocol.

The current raw-memory publication helpers mark a private allocation for GC;
they do not by themselves make every future GC cycle enumerate an unpublished
successor. Registry scanning must own every resource before a paused helper or
GC can encounter the installed attempt. A single physical bundle is an option
only if the existing allocation-registration and vector-header validation
contracts are adapted to recognize its interior vectors correctly.

### A descriptor-side 128-bit payload is useful, but not a source freeze

`EMPTY -> CAPTURING`, followed by ordinary key/value stores, creates another
owner-dependent window. An atomic transition publishing state and a TValue
payload together avoids that window.

However, atomic `{payload, state}` in the descriptor does not atomically couple
the payload to a separate source TValue. This attempted design is unsafe:

1. Publish payload A; helper H prepares `CAS(source, A, SRC)` and pauses.
2. Another helper observes a changed source B, replaces the intent payload with
   B, and starts another attempt using the same SRC token.
3. An old writer restores A in the source; H's delayed CAS succeeds.
4. SRC now resolves to B, although the value actually removed was A.

A phase reread immediately before H's CAS does not close this gap. Existing
writers separately validate the generation and CAS the TValue
(`lj_tab_keyed_store_commit()`, `src/lj_tab.c:6908` in the pre-fix tree;
`tab_trystoretv_cas_keyed_once_mode()` also performs a post-CAS currentness
check). RETIRING alone therefore does not prove that no old writer can perform
a delayed final CAS.

Likewise, preserving the source value during copying fixes the temporary GC
edge gap but does not establish the freeze linearization required by
`plan/06_concurrent_objects.md:190`. Discarding delayed old writes, or replaying
an observed write without a semantic argument, is not a replacement proof.

### Identity, destinations, and completion must also be helpable

The current marker payload identifies a descriptor only
(`src/lj_obj.h:261`, kinds at `:274`). Source slot address plus exact old-vector
identity can provide a deterministic source index. A destination marker needs
an equally unambiguous mapping back to its immutable source intent; descriptor
id alone cannot distinguish several migrating entries in one hash bucket.
Possible choices include an immutable per-destination owner sidecar or a token
namespace that encodes a distinct intent. The mapping itself cannot have an
unrecoverable claim-then-publish-pointer window.

The existing `tab_rehash_insert()` and `newhpart_publish()` use owner-local
free-node accounting and ordinary private writes (`src/lj_tab.c:1247` and
`:1087`). They cannot be called concurrently merely because source payloads
are durable. Destination reservation, collision linkage, payload publication,
and exact freecount publication need their own idempotent transitions. A
delayed helper must not use `CAS(nil, old_payload)` after the table has become
writable: a real deletion may have produced that nil, and the stale copy would
resurrect it. One-use exact destination tokens can distinguish initialization
from subsequent ordinary nil values.

A fetch-add cursor assigns work but does not prove it completed; a paused
claimant must leave recoverable work. Similarly, `slot=DONE; count++` has a
completion gap if the winner pauses between those actions. A monotonic
completion prefix, or a bitmap whose aggregation is itself helpable, avoids
making final cutover depend on one counter owner. The current public
`lj_tab_resize_desc_advance()` only validates adjacent phase numbers; production
phase transitions must enforce resource and completion invariants explicitly.

### Weak GC is a semantic part of migration

The registry scanner currently marks the descriptor allocation and retains its
table, then calls maintenance (`gc2_mark_tab_retired_mem()`,
`src/lj_gc2.c:3867`). It does not scan old/successor vector allocations or intent
payloads yet. That is sufficient only for the current metadata-only substrate.

Keep raw allocation retention separate from semantic child marking. Marking
every intent key/value as a root would strengthen weak tables. Follow the
existing table traversal and weak clearing rules, including strings' special
weak behavior, table metatable/`__mode` changes, and the current weak cycle.
Do not invent ephemeron behavior from the migration helper's conservative
`tab_resize_value_is_strong()` predicate: ordinary GC traversal has its own
key/value marking policy (`gc2_traverse_tab_rec()`, `src/lj_gc2.c:18622`).

The present weak pass treats FORWARD as incomplete work and retries
(`gc2_weak_process_tab()`, `src/lj_gc2.c:11659`, `:11701`). New markers must be
resolved or helped, not classified as ordinary lightuserdata or silently
ignored. A weak-clear success must prove that a source intent, unpublished
destination, or delayed helper cannot republish the cleared edge after WEAK
completes. A stale raw TValue must not be hashed or header-dereferenced without
the required exact incarnation admission. Vector SMR retains vector storage;
it is not a semantic lease on a weak referent.

An immutable payload and a mutable logical weak-clear result are different
facts. An eventual clear/rollback protocol must consume exact marker states so
a stale rollback helper cannot restore a payload after the collector cleared
it. Testing only strong tables would miss this essential conflict.

### Lifetime includes late stores, not just descriptor discovery

The current descriptor detachment and terminalization release the global VM
guard as soon as table control is stable
(`tab_resize_desc_finish_terminal()`, `src/lj_tab.c:1865`). Production vector
cutover needs the additional native/VM and vector-reader grace already called
out in the July note. A stable control word is not proof that every earlier
raw-vector user or helper has lost write authority.

Old vectors, descriptor identity, and immutable payload records must remain
retained until every possible late exact CAS is either completed or rendered
harmless. Descriptor addresses/intent tokens must not be reused within that
window. This includes cancellation, not only successful resize. The current
fallback descriptor registry lookup has a `LJ_ROOT_SCAN_LIMIT` bound
(`lj_tab_resize_desc_find_held()`, `src/lj_tab.c:2337`); if retired/cancelled
markers become operational, their lifetime/discovery design must also explain
how all retained identities remain findable under a long-lived reader and a
large number of later attempts.

## Candidate: immutable capture and whole-attempt abort

The following is a candidate for a small executable model. It has not been
proven, modeled, implemented, or selected as the final protocol.

1. Preallocate the descriptor, successor vectors, and one immutable capture
   record per possible source slot. Publish resource ownership, install exact
   table control, and validate the sizing/source assumptions.
2. Publish each slot's payload with an atomic state/payload transition. Do not
   overwrite or recycle that record after publication. Hash keys need their
   own exact publication/identity argument.
3. Freeze each source using `CAS(captured_payload, SRC(exact_attempt))`.
   The source position determines the intent. Every helper uses the same
   immutable payload; no helper owns a private lost value.
4. If any source changed before it could be frozen, choose a whole-attempt
   abort before commit. Helpers restore only exact markers belonging to this
   attempt, using the immutable payload and the eventual weak-clear protocol.
   A failed source CAS is evidence of a competing write, rather than a reason
   to mutate/reuse the capture record.
5. Once every source is frozen, atomically commit the attempt and forbid
   abort. Build and publish the successor using separately proved idempotent
   destination operations. Writers that encounter the descriptor help finish
   or abort it before retrying their own operation.

This could bound per-attempt storage without assuming a bounded number of
source value changes: an attempt either freezes its fixed captures or aborts
as a whole. It also avoids a paused CAPTURING owner. Those advantages are not a
proof of lock-free progress or semantic correctness.

Before choosing this design, the model and runtime reasoning must resolve at
least these cases:

- **Late source CAS after abort.** A helper may pause before its source CAS,
  lose to an abort and full rollback, then install the old SRC into an ordinary
  source value after table control is stable again. The aborted descriptor
  must still be findable and restore/resolve that exact marker. Its SMR lease
  must extend through the CAS and post-check; terminal reclamation and VM guard
  release need a proof that no unresolved marker or late write authority can
  survive their grace conditions.
- **Retirement flags after abort.** An ordinary delayed OR of RETIRING has no
  descriptor identity and could strand an otherwise cancelled generation.
  Before commit, use descriptor control as the proposed structural exclusion,
  or design an exact reversible header transition. Merely clearing a flag
  during abort is not enough.
- **Nil-key and delayed new-key publication.** An initially empty hash slot
  may be sampled before another thread publishes its key or links the node.
  Freezing the nil value must prevent that inserter from publishing a live
  value into the old generation, and the inserter must re-resolve its operation.
  Existing anchor/collision insertion and FINREG paths have different
  publication sequences (`tab_newkey_impl()` and
  `lj_tab_try_newkey_anchor/chain()`); none can be assumed covered by a value
  capture alone. A paused KEYLOCK/FINCLAIM must not become a new mandatory wait.
- **GC clearing during freeze or rollback.** A delayed rollback must not
  resurrect a weak value. A delayed destination copy must not revive an edge
  after weak completion. An exact one-use NIL_DONE transition is a possible
  component, not yet a complete source/destination clear protocol.
- **Destination collisions and delayed helpers after success.** Finish
  linkage, marker removal, completion accounting, and unused-slot initialization
  without owner-only writes or any post-publication nil ABA.
- **Cancellation progress and cost.** Repeated aborts must correspond to
  actual competing progress, not two resizers livelocking each other. Allocation
  and retry costs must be measured against the existing production path after
  the protocol is correct; full-table abort is potentially expensive.

The next useful proof is a small schedule model that pauses every capture,
source CAS, abort, destination publication, weak clear, and terminal/grace
boundary. It should include A->B->A source writes, same-valued later
generations, nil deletions, delayed key linkage, and paused initiators. A passing
model would justify a cohesive runtime implementation, including readers,
writers, GC, and reclamation, rather than another dormant phase-only extension.

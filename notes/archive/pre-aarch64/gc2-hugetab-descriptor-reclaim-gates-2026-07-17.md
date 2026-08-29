# GC2 HugeTab descriptor reclamation gates (2026-07-17)

This checkpoint extends the dormant GC2 table-descriptor/token authority from
small arenas to HugeTab-managed mappings. Descriptor producers are still not
enabled. The checkpoint establishes the reciprocal lifetime rules and
deterministic fault coverage needed before they can be enabled.

## Pointer-form certificate

HugeTab metadata is the lifetime locator and classifier. A non-traversable
entry cannot publish GC2 table-scan authority, so reclamation never derives or
dereferences its mapping header. This is both the fast path and necessary for
metadata-only HugeTab users which use synthetic addresses.

For a traversable entry:

- a normal huge body uses the exact pointer certificate: its embedded table
  token must be `NONE`, and `ACTIVE` vetoes only that exact body;
- a small-arena registry key is the aligned mapping base, so its certificate
  requires every embedded token to be `NONE` and the descriptor to be clear
  for the whole mapping;
- an unrelated `ACTIVE` identity permits progress; and
- `PINNED`, malformed descriptor state, or inconsistent attachment fails
  closed for the mapping.

Classification is driven by slot flags and pointer geometry. It never samples
`LJ_AF_HUGE_MAGIC` merely to decide whether dereferencing an address is legal.

## Destruction and locator gates

Every mapping/header certificate read is now made while the exact HugeTab slot
contains one real lifetime owner. Ordinary runtime destruction first adds a
counted internal reader with a full-slot CAS. A destructive CAS succeeds only
if that count is still exactly one and atomically consumes it into either
`FREEING|BUSY` or the tombstone. Thus a new reader, root, recovery identity, or
lawful descriptor publisher changes the same metadata word and defeats the
destructive transition.

The certificate gates:

- pre-destructor acquisition and its post-claim crossover recheck;
- post-grace `RETIRED -> FREEING` ownership;
- external-free claim and finish;
- deferred-free folding and recovery-terminal discard;
- ordinary delete and terminal forget;
- terminal `fini_all` entry removal;
- dead-owner transfer; and
- direct huge unmap as the final independent backstop.

`lj_arena_hugetab_fini()` now releases only an actually empty table. Any live
slot remains an authoritative locator, regardless of its transient flags.
Joined-world `fini_all`, terminal forget, and dead-owner transfer use the same
counted certificate lease but may adopt abandoned `BUSY/FREEING` metadata.
Those three operations retain their existing outer TG-writer/joined-world
precondition; a slot pin does not by itself make the `HugeTab.h` wrapper safe
against concurrent whole-table destruction.

The pre-destructor late-veto rollback loops on the exact 128-bit slot. It
clears only this owner's `BUSY|FREEING`, preserves a racing durable
`DEFER_FREE`, preserves all unrelated metadata changes, and conservatively
restores pre-claim `MARK`/`RETIRED` liveness with `MARK` dominant. It cannot
strand an opaque `BUSY` owner after a metadata race.

Dead-owner transfer is permitted to remove the source locator only after the
destination locator has been inserted or confirmed. The internal terminal
forget therefore has an explicit alternate-locator proof; the public
terminal-forget API has no such exemption.

## Sweep iterator versus body admission

`lj_arena_hugetab_sweep_next()` is enumeration only. Its returned metadata may
be stale immediately. `lj_gc_reclaim_gc2_huge()` therefore ignores that
metadata for body authority and performs a fresh
`lj_arena_hugetab_sweep_reader_acquire()` full-slot CAS before reading
`retire_obj`, `retire_epoch`, an object header, a trace, or any payload byte.

The exact sweep admission requires `SWEEP_OLD`, rejects `DEFER_FREE|BUSY`, and
requires `TRAVERSABLE|READY` for nonterminal bodies. Stable `FREEING` entries
are admitted because their allocator-header grace epoch still has to be read.
The reader is held across every body/header inspection and is released before
zero-reader ownership transitions such as retire, live-ticket claim,
`RETIRED -> FREEING`, and delete. The later transition revalidates the exact
metadata; a free in the release-to-claim gap therefore wins safely.

`finish_sweep()` now takes the same counted reader before sampling
`retire_obj`. It keeps that reader through the final header stores and releases
it afterward, so a racing external free can only publish `DEFER_FREE` and is
folded by the release.

## Durable nonblocking handoff

An external free is irrevocable even when a descriptor or token owns physical
scan access. It therefore never waits and never leaves `BUSY` dependent on the
departed caller. It publishes `DEFER_FREE`; descriptor-blocked states also
carry `SWEEP_OLD`, making the retry discoverable by the bounded huge sweep.

While `DEFER_FREE` is present, GC2 huge sweep first attempts only
`lj_arena_hugetab_retry_deferred()`. Even if an iterator raced a new DEFER, the
fresh sweep-reader CAS rejects, so no stale hint can authorize payload access.
The retry itself takes a counted certificate lease; it never samples the
mapping from a naked lookup. It succeeds only after readers, root and recovery
identities, `BUSY`, the exact token, and the descriptor certificate are clear.
Its successful full-slot CAS:

- clears `DEFER_FREE`, `MARK`, `RETIRED`, and `BUSY`;
- publishes `FREEING|SWEEP_OLD`;
- release-publishes a fresh-grace sentinel first; and
- wakes sweep progress.

This polling hook is intentionally exposed as an internal debugging/progress
API. It permits deterministic tests and later descriptor-completion helpers
without adding a lock or wait to descriptor release.

Fold progress distinguishes `NONE`, certificate-`BLOCKED`, newly
`SCHEDULED`, and `FREEING`. A real last-owner release wakes for all non-NONE
results. A synthetic retry wakes only when it newly schedules the entry or
publishes FREEING, avoiding a retry wake storm on an unchanged blocked marker.

## Certificate cutoff and claimed unmap

The successful certificate observation under an exclusive counted admission,
followed by the exact destructive slot CAS, is the reclamation cutoff. A
lawful later table authority publisher must first add a reader/root/recovery
owner and therefore makes that CAS lose. `PINNED` or malformed state present
before the cutoff still fails closed.

A raw global fault CAS after the cutoff cannot retroactively revoke it: no
finite postcheck across two independent words can exclude a pin immediately
after the final sample. Such raw asynchronous injection is therefore
joined-world/debug-only. Defensive postclaim samples remain useful fault
containment, but are not the production synchronization proof.

After tombstoning the authoritative locator, production callers use
`lj_arena_huge_unmap_claimed()`. It trusts the already-consumed certificate
lease and cannot strand an unlocated mapping because an unrelated prospective
global fault pin appeared later. `lj_arena_huge_unmap()` remains the checked
backstop for direct/unclaimed mappings and deliberately retains a mapping when
authority was already present.

The descriptor/token primitives are still dormant infrastructure: production
initializes and attaches the descriptor, but has no production ACTIVE/PENDING
publisher yet. The active table scan proof remains the table stamp under a
`GC2MarkScope`, whose huge form already holds a counted reader. Before dormant
descriptor publication is enabled, it must be wrapped in one scoped publisher
API which holds small rescue or huge reader admission from ACTIVE publication,
through token refresh and descriptor completion, until release. Missing
locators and saturation must pin reclamation rather than forge completion.

## Deterministic validation

The HugeTab fixture installs a real local descriptor and embedded huge token.
It covers exact `ACTIVE` and token `PENDING` at destructor, post-grace claim,
delete, claim-time external free, defensive raw post-claim injection, retry,
`fini_all`, and direct unmap. It also covers unrelated `ACTIVE`, injected
`PINNED` and malformed states, and proves plain huge mappings ignore impossible
GC2 authority.

The exact sweep-reader fixture deterministically exercises both CAS orders:

- iterator hint, ordinary reader, external DEFER, rejected sweep admission;
- sweep admission, external DEFER, final-reader FREEING handoff.

The raw post-claim injection case is explicitly fault-containment coverage. It
bypasses the publisher admission future production code is required to hold;
it is not evidence that such an interleaving is lawful.

Validated commands at this checkpoint:

- `src/luajit tools/test.lua m2_arena_hugetab` with GCC;
- `CC=clang src/luajit tools/test.lua m2_arena_hugetab`;
- normal GCC and Clang `make -C src -j2` builds;
- `src/luajit tools/test.lua m2_arena_gcsweep`;
- `src/luajit tools/test.lua m2_arena_all`;
- the HugeTab fixture under ASan+UBSan; and
- the HugeTab fixture under TSan (GCC emits its known warning that standalone
  `atomic_thread_fence` operations are not instrumented; the run itself was
  clean).

## Outer teardown status propagation

Fail-closed entry retention is useful only while every enclosing owner remains
authoritative. The audit found that the old void finalizers could retain an
inner HugeTab or arena list while a caller cleared/freed the `TGAlloc`,
`TGState`, small-registry wrapper, or final `GG_State`. The completed follow-up
uses additive checked APIs, preserving the old symbols and their calling ABI:

- `lj_arena_hugetab_fini_try()` reports DONE only when `ht->h` is gone;
- `lj_arena_hugetab_fini_all_try()` reports partial unmap progress separately
  from DONE/BLOCKED;
- `lj_arena_alloc_fini_try()` reports DONE only when all four list classes for
  every kind are empty, retaining exact list heads and `smalltab` on BLOCKED;
- `TG_FINI_RETRY` keeps a TG body, legacy-list link, stable `RECLAIMING` slot,
  and any worker-retire/raw owner until a later attempt publishes DONE; and
- every worker/threading path which immediately frees TG storage now requires
  checked DONE first.

The small-arena directory now outlives raw GC2 teardown. Terminal secondary
and main allocators delete their exact entries while all `TGAlloc.smalltab`
aliases and the global wrapper remain published. Only an empty checked
HugeTab result clears those aliases and frees the wrapper. A `REGISTERED`
mapping with a missing directory now fails closed; an actually unregistered
private/bootstrap arena may still unmap without a directory lookup.

Main close finalizes the authoritative in-place `g->main_tg->alloc`, not a
stack copy. Partial progress therefore cannot leave the original heads naming
unmapped arenas. The internal x64 GG is a separate huge mapping, is forgotten
from its HugeTab before table destruction, and remains mapped until checked
main-allocator and small-registry completion.

There are two terminal preflight strengths:

- the early joined-world certificate pass requires descriptors/tokens clear
  but permits root/recovery/destructor owners which `freeall` itself consumes;
- the final TG/allocator pass additionally requires those remaining lifetime
  owners clear before physical wrapper destruction.

The early pass is ordered before terminal recovery discard. It first rejects
every counted HugeTab reader, descriptor/token veto, and non-discardable
recovery/defer combination across the small directory and every TG allocator.
Only that complete certificate may consume recovery identities. Durable remote
free queues are force-drained afterward and their exact gates are rechecked.
Thus a retry never inherits a recovery locator which an earlier blocked pass
partially destroyed.

A provisional TG whose `pthread_create` never succeeded has no registry owner.
If its checked runtime teardown is blocked, its `threading.thread` live node is
now the explicit retry owner. Shutdown retains that exact `TG_FINI_RETRY`
shape; all-userdata finalization may retry but cannot discard it; after GC2
workers join, a strict terminal live-node drain either publishes DONE and then
clears/frees every pointer, or fails before close mutates recovery/native-root
state. The unstarted child stack is also returned directly through its known
`tg->allocd`. Generic pointer routing cannot discover an unattached TG and used
to risk inserting child-owned small memory into main bins (or consulting the
wrong HugeTab) before the child allocator unmapped it.

`lua_close(void)` remains a one-shot ABI. A persistent injected `PINNED` or
malformed descriptor therefore fails closed at the early preflight rather than
reaching a tail UAF. If fault-state shutdown itself must become resumable, add
an additive `lua_close_try`/debug terminal-drain API with an explicit close
stage before finalizers/freeall; changing the existing `lua_close` signature is
not ABI-compatible.

Deterministic coverage now also proves:

- blocked checked HugeTab/allocator finalization retains its owners and later
  reports DONE distinctly from a successful zero-unmap result;
- a real huge reader drives a terminal worker TG to `TG_FINI_RETRY`, with both
  legacy-list and worker-retire ownership preserved, then retries to DONE and
  unmaps exactly once; and
- an early small-registry finalization attempt preserves the global pointer,
  `TGAlloc.smalltab`, backing table, and a known exact lookup until close drains
  the registered allocators; and
- injected `pthread_create` failure can meet two consecutive checked teardown
  vetoes (spawn cleanup and all-userdata finalization), remain live-rooted, and
  finish on the joined-world third try. The complementary one-veto case proves
  a successful all-userdata retry also tombstones that retained live node.
  Separate small and 3000-argument huge child stacks are exercised while the
  main allocator churns before close.

The current outer-teardown follow-up passes `m2_arena_all`,
`m4_tg_terminal_orphan`, `m4_threading_lifecycle`,
`m4_threading_spawn_native`, and `m3_gc2_worker_scheduler` (JIT off and on).
The final HugeTab fixture also passes Clang, ASan+UBSan, and TSan (with only
GCC's documented uninstrumented-fence compile warning demoted), while the
injected spawn-failure fixture passes both Clang and Valgrind.

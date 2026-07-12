# GC2 no-drop traversal recovery (2026-07-12)

## Why this exists

GC2 graph publication previously treated the SSB and grey deque as if they
could not run out of capacity. Debug builds asserted when publication failed,
but release builds could consume the source entry without retaining the graph
identity. That is not a safe degradation: a missed container traversal can
make reachable descendants appear dead.

The replacement rule is now: a semantic source is consumed only after the
same object identity is durable in either its normal queue or an exact,
allocation-free recovery plane. Failure to classify an identity is sticky and
fail-closed; it prevents weak clearing and every reclaiming sweep boundary.

## Representation

- Small arena allocations use an atomic packed two-bit state per start cell:
  `IDLE`, `PENDING`, `CLAIMED`, or `REDIRTY`.
- Huge allocations use the same state machine in HugeTab metadata.
- The embedded main `lua_State` has a dedicated state word because it is not an
  arena allocation.
- `recovery_items` is an exact global close veto. A publisher reserves it
  before the per-object `IDLE -> PENDING` linearization and rolls the reservation
  back if another publisher won.
- `recovery_failed` is a sticky semantic veto for an identity which cannot be
  represented. It also pins typed activation to `NO_RECLAIM`, but semantic
  phase predicates do not rely on that physical-sweep veto alone.

`CLAIMED -> REDIRTY` coalesces a mutation racing a traversal without allocating
or losing the second pass. Completion changes `REDIRTY` back to `PENDING` and
keeps the global item count; only a completed, non-redirtied traversal changes
the object to `IDLE` and decrements the count.

The worker's packed lane cursor keeps the current main/small/huge lane for at
most 64 successful replays before rotating. This makes fairness starvation-
bounded while amortizing directory-sized empty HugeTab scans. Per-object
rotation made the 24-state allocation churn fixture take more than four
minutes; the bounded quantum completes the same fixture in roughly 28 seconds
on the development container.

## Queue transaction rules

- SSB publication is tried first on mutator paths. A full embedded SSB no
  longer allocates a dynamic node from libc; it falls back to recovery.
- Grey growth uses the non-throwing allocator and starts with a 256-entry
  embedded ring. Failed growth falls back to recovery.
- A published SSB slot/count is cleared only after grey or recovery publication
  commits. Test hooks pause at that destination-committed/source-retained
  interval.
- Grey span corruption no longer repairs by advancing the consumer cursor and
  dropping identities. It sets the sticky fail-closed veto and leaves the
  offending span observable.

The common wrappers cover mutator, worker, remembered-set, root-rescan,
thread, table, and SWEEP-rescue publishers. The embedded main thread is
traversed explicitly rather than being rejected by arena validation.

## Lifetime and free interaction

Small and huge recovery state is an allocation lifetime pin. Sweep/free cannot
reuse, transfer, or unmap a recovery-bearing identity. A free which collides
with recovery records deferred free ownership:

- small allocations retain late/deferred state until recovery completion;
- huge allocations set `DEFER_FREE` and recovery completion hands them to
  `FREEING|SWEEP_OLD` with a fresh grace period;
- a busy HugeTab retirement collision is requeued without changing the exact
  recovery count;
- terminal shutdown reconciliation either clears a normal identity or
  tombstones a deferred-free mapping and takes explicit unmap ownership.

See `gc2-arena-recovery-substrate-2026-07-12.md` for the lower-level arena
state and free/rebuild invariants.

## Phase and reclamation gates

The full empty predicate now includes active and published SSB work, grey work,
`recovery_items`, and `recovery_failed`. It gates MARK closure, MARK-to-WEAK,
weak-frontier closure, WEAK-to-SWEEP, sweep bridge publication, physical arena
sweep, and SWEEP-to-IDLE.

Two irreversible boundaries have their own last-mile checks:

- weak snapshot clearing rechecks recovery both before and after advertising a
  weak-drain owner, so a reserve-before-locator window cannot advance the clear
  cursor;
- SWEEP worker progress requires the complete work predicate before grace,
  string sweeping, or owner-arena sweeping. A zero-result recovery scan alone
  is not treated as emptiness.

## Verification hooks and tests

`LJ_GC2_TEST_HELPERS` provides deterministic failure/pause points for grey
growth, recovery reservation, pre-completion, and SSB destination commit. The
focused recovery fixture covers:

- a completely full active SSB with no replacement node;
- grey growth failure while the SSB source remains published;
- the reservation gap vetoing phase closure;
- `CLAIMED -> REDIRTY -> PENDING` replay;
- weak clearing remaining unchanged across a reserved recovery item.

The fixture is built release-like and with GC2 paranoia enabled. Arena tests
also cover late small frees, huge deferred free, busy retirement requeue,
terminal reconciliation, and transfer/rebuild retention.

Integration runs also exposed and fixed two independent reachability bugs:

- the JIT PC-to-prototype root-spine probe used a marking validator before it
  checked the candidate type, which falsely kept unrelated FINREG cdata alive;
  it now performs non-marking observed-object validation and only the matching
  prototype is marked by its caller;
- parser and bytecode-reader prototype constructors allocated reusable
  unlinked arena storage without resetting the GC flag byte, allowing stale
  `FIXED`, `SFIXED`, FINREG, or NEEDSCAN bits to survive READY publication;
  both constructors now publish an exact new-white flag value first.

## Deliberate remaining work

- Huge recovery scanning keeps a persistent slot within a HugeTab, but a
  persistent round-robin cursor across thread groups would strengthen fairness
  under a workload which continuously redirties early tables.
- Huge publication currently uses a bounded, nonwaiting SMR lookup. A transient
  lookup failure is safe (sticky fail-closed) but pessimistic; passing an
  already-held HugeTab/admission hint would avoid turning that rare collision
  into a permanently vetoed cycle.
- Recovery makes traversal loss safe; it does not by itself prove all GC2,
  JIT, FFI, VM, HugeTab-rehash, sanitizer, or cross-platform release gates.
  Those remain required before `b1.2.0`.

## Integration hardening found by the first full-runtime runs

The allocation-free recovery integration made several older ownership and JIT
costs deterministic enough to diagnose. These are runtime fixes around the
substrate, not changes to `plan/`.

### Retired JIT work and grace progress

Arena quarantine revisits at most 64 cells per owner step. Re-running the full
semantic retired-trace preservation walk on every failed physical-free retry
therefore multiplied root-spine work and made stock test 393 appear hung only
when executed after the preceding 392 cases. Reclaim requeue now preserves the
raw trace/exittab allocation on both sides of list publication; the original
retirement edge and each cycle's retired-root pass remain responsible for the
KGC/prototype graph. Trace and mcode retired lists memoize an all-young scan for
one completed epoch, but ready bodies blocked by an inbound trace, debugger
publication, slot release, body validation, or an mcode reference remain
eligible for same-epoch retry.

A pending trace needs `LJ_FLUSH_EPOCHS`, not merely one generic arena grace.
Every small and huge quarantine observation of an intact retired trace now
reasserts `sweep_grace_needed`; otherwise the owner can exhaust the one grace,
rescan the still-young trace forever, and never advance the handshake epoch.
Focused fixtures cover raw-only requeue, all-young memoization, same-epoch
transient retry, and destruction only at `E + LJ_FLUSH_EPOCHS`.

### Snapshot-PC prototype ownership no longer scans `gc.root`

The first optimization bounded retired snapshot-PC searches to 8192 shared
ownership-spine entries. That was fast but not lossless: exhaustion silently
left later prototype bytecode owners unmarked, while semantic requeue had been
removed from the hot quarantine retry.

On the supported arena allocator, `proto_bc(pt)` immediately follows the
`GCproto` header. A raw snapshot PC can therefore be resolved directly through
small-arena allocation coverage, or a huge allocation range lookup, then
admitted as the exact `~LJ_TPROTO` allocation under the existing rescue/SMR
lease. Live GC2 trace traversal and retired-trace preservation now use this
bounded allocator lookup. The cache, global walk budget, and three separate
root-spine PC searches are gone; no snapshot owner is dropped because an
unrelated root list was long. The temporary ignored-custom-`lua_Alloc`
boundary remains conservative because those allocations are not reclaimed.

### Pending-root publication and FINREG double membership

The main-TG "single threaded" pending-root fast path sampled the activation
counters, loaded a pending head, then used a plain release store. A worker or
attacher can become visible and exchange the stack between that load and store.
The later store republishes a detached tail whose arena address can be retired,
reused as an interned string, and finally overwritten through the stale `gcw`
link. Every C ordinary/chain/after-main publisher now couples its acquired head
to publication with a CAS retry even before multithreading becomes sticky.

Interned strings are never ownership-spine nodes: `nextgc` is their hash-chain
link. Root validation, unlink, sweep, trace fallback, and shutdown now reject a
string before reading that word. An inadmissible incoming root edge is severed
without following its foreign successor, and trace reclaim frees only after an
exact unlink or a complete valid scan proves absence.

The concrete full-stock corruption was an I/O userdata with a registered
finalizer. Root pruning could detach it and create an arena LIVE/RETIRED
reanchor ticket; finalizer dispatch then inserted the same intrusive object
after the permanent main-thread anchor. The ticket later inserted it globally,
forming a cycle/two incoming memberships. Sweep now keeps
`LJ_GC_UDATA_FINREG` userdata attached and physically marked until dispatch
requeues it and only then clears FINREG, matching cdata's ownership rule.
Main/vm threads are permanent non-prunable anchors, and the after-main flusher
no longer walks a live chain to obtain an unleased tail for defensive reanchor.

After these changes an assertions-plus-GC2-paranoia build completed the 509
stock tests ten consecutive times. This is an x86_64 Linux integration result,
not a cross-platform or release declaration.

The remaining direct x64 publishers and FINREG unlink proof are now closed.
Interpreter TNEW and traced inline FNEW both publish with a retrying locked
`CMPXCHG`; a concurrent pending-stack exchange can no longer detach the sampled
head and then have a stale tail stored over it. The real VM path has a
deterministic load/exchange/CAS regression, and the FNEW fixture inspects the
generated machine code as well as running the allocation path. Cdata and
userdata FINREG discovery use the shared tri-state root unlink result:
`UNLINKED` and a complete valid proof of `ABSENT` permit enqueue, while
`UNPROVEN` retains both object and callback, restores a claimed cdata slot, and
leaves the side-list identity active for retry. Active userdata FINREG nodes
also keep the close loop pending.

On the final shared source set, a fresh assertions-plus-GC2-paranoia build
completed five more consecutive 509-test stock runs. The focused pending-root,
recovery (release-like and paranoia), traced FNEW, trace/mcode retirement, and
complete M2 arena suites also passed. The x64 publisher audit checked the
generated DynASM control flow and both SysV and Win64 register/home-area rules.

A clean release-mode Linux build then completed five consecutive 509-test stock
runs; the measured run took 5.42 seconds wall-clock in the development
container. Isolated frozen-source portability builds and runtime smokes also
passed on both remaining supported targets. Windows x64 UCRT under Wine and
macOS x64 under Darling each exercised FFI, hot JIT loops, collection, and four
concurrent `threading.spawn` workers. These are focused integration smokes, not
the complete cross-platform release gate for `b1.2.0`.

One structural debt remains even though every audited current caller satisfies
its exclusivity invariant: generic `lj_gc_linkobj*` does not encode an explicit
membership lease, and a small-arena LIVE reanchor publishes before its final
`LIVE -> WHITE` CAS. A dedicated membership/reanchor claim would turn that
caller convention into a mechanically enforced invariant and is the next root
ownership hardening slice.

### Enlarged-global JIT constant layout

GC2 metadata has grown `global_State` beyond the JIT's 10-bit GG-relative
`IR_FLOAD` fold-key reach. Keeping SIMD constants inside the later `jit_State`
made ABS/NEG recording assert depending on which traces became hot. The two
immutable 128-bit constants now live in an explicitly aligned slot near the
front of `global_State`; `jit_State` no longer duplicates that storage. Their
GG-relative offset is 192 bytes in the current x64 layout, preserving the
compact IR encoding and avoiding a wider instruction or extra runtime load.

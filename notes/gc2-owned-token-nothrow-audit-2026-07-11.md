# GC2 owned-token allocation and nonlocal-exit audit (2026-07-11)

Status: audit of committed checkpoint `17dbe813`.  No production source was
changed by this audit.  The plan files were not edited.

This is a source-reachability audit, not a vague warning that an allocation
could theoretically fail.  The committed source contains throwing allocation
paths while GC2 owns global progress tokens, and some of those paths mutate or
detach the only representation of mark work before the throw.  Consequently a
generic protected-call wrapper is not a sufficient fix: it could release the
token while still allowing a reachable object to be reclaimed.

The shared worktree acquired a separate, uncommitted MARK-intent repair while
this note was being written.  That repair is called out below, but all line and
behavior claims in the main audit refer to `17dbe813`.

## Confirmed throwing allocation primitives

`lj_mem_newvec()` expands to `lj_mem_realloc()`.  On allocation failure,
`lj_mem_realloc()` calls `lj_err_mem()` and nonlocally unwinds.  GC2 uses this
throwing primitive in three owned data structures:

1. `gc2_grey_grow()` allocates the replacement grey ring.
2. `gc2_weak_resize()` allocates `weak_stack`, then `weak_ready`.
3. `gc2_finclaim_ensure()` allocates the cdata object vector, then the
   finalizer-value vector.

The last two functions also have a local leak before any GC2 token cleanup is
considered: if the first allocation succeeds and the second throws, the first
allocation has not been published and is no longer reachable.

The selected `lua_State` is itself unsafe.  `gc2_grey_grow()` falls back to
`mainthread(g)` when a parked worker has no current Lua state, and
`gc2_weak_resize()`/`gc2_finclaim_ensure()` always use `mainthread(g)`.  With
the internal owner-local allocator, a background worker or secondary mutator
therefore allocates through the main TG's allocator cursor.  A successful
allocation can remotely race the main TG; an unsuccessful allocation attempts
to unwind the main Lua state from the wrong OS thread.  This is a correctness
bug independent of OOM.

## Exact `worker_active` failure traces

The following committed call chains all reach `gc2_grey_grow()` after
`worker_active` has been acquired:

| Owned operation | Concrete path | Additional state stranded by a throw |
| --- | --- | --- |
| Cycle start | `gc2_mark_begin()` -> `gc2_worker_claim()` -> publish MARK -> `gc2_grey_grow()` | `worker_active=1`, phase MARK, consumed cycle request |
| Ordinary worker | `gc2_worker_drain_inner()` -> SSB conversion or `gc2_traverse_obj()` -> `gc2_grey_push()` -> grow | worker token plus the work state described below |
| Mutator assist | `lj_gc2_assist()` -> worker claim -> `assist_active=1` -> drain/traverse -> grow | worker token, `assist_active=1`, and `tg->gc_assist=1` |
| MARK close | `gc2_mark_close_help()` -> `gc2_worker_claim_mark_close()` -> fixpoint drain -> grow | worker token and `mark_close_intent=1` |
| WEAK close | `lj_gc2_weak_complete()` -> worker claim -> `gc2_weak_mark_close_round()` -> owned drain -> grow | worker token; if the first close snapshot is being taken, `weak_root_scanned=2` |
| WEAK -> SWEEP | `lj_gc2_weak_to_sweep()` -> worker claim -> phase CAS to SWEEP -> boundary drain -> grow | worker token, phase SWEEP, bridge not ready |
| SWEEP bridge | `lj_gc2_sweep_prepare_bridge_boundary()` -> worker claim -> root-work drain -> grow | worker token and an incomplete semantic bridge |
| SWEEP close | `lj_gc2_sweep_to_idle()` -> worker claim -> set `cycle_leader=LJ_THREAD_GCSCAN` -> final SSB drain -> grow | worker token and cycle-close gate |
| SSB flush fallback | `gc2_recycle_published_ssb_for_flush()` -> worker claim -> published SSB conversion -> grow | worker token and SSB consumer state |
| Root/table fallback | `gc2_root_rescan_later()` or `gc2_table_rescan_later_()` -> failed SSB push -> worker claim -> grey push -> grow | worker token and a marked/NEEDSCAN object whose publication did not finish |
| Explicit SSB drain | `lj_gc2_drain_ssb()` -> worker claim -> owned drain -> grow | worker token and SSB consumer state |

`gc2_mark_begin()` independently reaches both throwing two-vector allocators
while retaining `worker_active`: `gc2_weak_reset()` may call
`gc2_weak_resize()`, and `gc2_finclaim_reset()` may call
`gc2_finclaim_ensure()`.  Weak-vector resizing is not just a first-cycle case;
the next cycle adaptively grows it from the preceding weak count.

`gc2_finclaim_prepare()` can also call `gc2_finclaim_ensure()` from cdata
P_WEAK discovery.  Today a successful first cycle normally leaves that fixed
vector ready, but the helper itself is not safe to call from an owned close and
must not retain a throwing contract.

## Why token cleanup alone is incorrect

### Published SSB chain is detached and its slot is destroyed first

`gc2_drain_published_ssb_to_grey()` increments `ssb_consumer_active`, removes
the chain from `ssb_drain`/`ssb_head`, then, for each item:

1. loads the object;
2. clears the source slot;
3. decrements and publishes the node count;
4. calls `gc2_ssb_mark_one()`;
5. that function may call `gc2_grey_push()` and throw while growing.

At that point the current object has no SSB representation, the rest of the
chain exists only in a C local, `ssb_consumer_active` is nonzero, and
`worker_active` is nonzero.  Releasing just the two counters loses the current
object and the detached suffix.

The active-SSB path has the same ordering problem: it clears the active slot
before `gc2_ssb_mark_one()` and only retreats the TG cursor after marking.

### Grey parent is popped before child publication

`gc2_worker_drain_inner()` and `gc2_drain_grey()` pop and clear a grey object
before calling `gc2_traverse_obj()`.  A large table, prototype, trace, function
or thread may fill the ring while its children are being discovered.  Child
marking sets the arena mark before `gc2_grey_push()` allocates.  If growth
throws, the parent has already left the deque and its payload is only partly
traversed; the child which triggered growth is marked but not queued.

Several release builds merely discard a nonthrowing push failure because the
only enforcement is `lj_assertG(pushed, ...)`.  Replacing
`lj_mem_newvec()` with a nothrow allocator without adding durable recovery
would therefore turn a token leak into silent reachable-object reclamation.

### Counted readers are leaked as well

The same unwind can bypass scopes nested inside the worker token:

- `gc2_traverse_obj()` holds an arena rescue admission until
  `gc2_mark_scope_leave()`;
- `gc2_traverse_thread()` can hold both a `LJStateClaim` and a GC2 SMR reader
  while its stack/env/upvalues enqueue children;
- published-SSB conversion holds `ssb_consumer_active` and a detached chain.

Thus even forcibly clearing `worker_active` after an error leaves arena
reclamation, SMR reclamation, or SSB completion permanently denied.

## Snapshot and close-token results

### `weak_root_scanned=2` has a direct throw path

`gc2_weak_mark_close_round()` performs `0 -> 2` before its pre-snapshot owned
drain.  That drain reaches the throwing grey growth path.  An OOM can therefore
leave `weak_root_scanned=2`, `worker_active=1`, and potentially a detached SSB
suffix simultaneously.  All later weak-close attempts reject state 2.

### `mark_root_scanned=2` does not currently enclose `lj_mem_newvec()` directly

`gc2_mark_root_snapshot()` publishes state 2 immediately before the root
handshake.  The current owner/global root scanners publish object work through
SSBs rather than the worker grey ring, so under the temporary internal
allocator policy I did not find a direct `lj_mem_newvec()` call between this
CAS and the `2 -> 1/0` CAS.  This is not a helpable state, however: a suspended
snapshot/handshake leader leaves every peer returning on state 2.  It also
depends on every future handshake action remaining nothrow.

### `mark_close_intent` had a separate stale-phase preemption window

In committed `17dbe813`, successful MARK close released `worker_active` before
clearing phase-local `mark_close_intent`.  If the helper stopped after
publishing WEAK and releasing the worker token, ordinary worker claims rejected
the still-set intent, while the worker dispatcher only called the intent helper
when its sampled phase was MARK.  No peer repaired the bit in WEAK.

The shared worktree now clears intent before worker release and makes ordinary
claims phase-gate the bit.  That is the correct local repair and should be kept.
It does not address a throw while the close helper still owns
`worker_active`.

## Existing nonthrowing allocations which still lose semantics

### Dynamic SSB node OOM

`gc2_ssb_new_dynamic()` uses `malloc()` and returns NULL.  If a full active SSB
has no free replacement, `gc2_flush_ssb()` and then `lj_gc2_ssb_push()` fail.
Many root, barrier and sweep-rescue callers have already marked the target and
only assert that the push succeeded.  With assertions disabled, the newly
marked container can have no traversal work item.  A persistent root snapshot
need not revisit it, so this is not merely delayed work.

### Weak-overflow node OOM

`gc2_weak_overflow_push()` is deliberately nonthrowing, but
`gc2_weak_record()` ignores its return.  Production weak completion passes a
NULL legacy bridge head.  Therefore a weak table beyond the vector capacity,
or any weak table when the vector is unavailable, has no recoverable identity
if overflow-node allocation fails.  Completion can still report success while
never weak-clearing that table.  This violates Lua weak-table semantics even
though it conservatively retains memory.

### Finalizer queue node OOM aborts the process after ownership mutation

`gc2_finalizer_node_new()` calls `abort()` on `malloc()` failure.  P_WEAK
userdata/cdata discovery reaches it while `worker_active` is held.  The callers
may already have claimed/cleared the FINREG slot, unlinked the object from the
root spine, set FINALIZED, and/or retired its side-list record.  Changing the
abort to `return NULL` without reordering those mutations would lose the
finalizer and its last ownership record.

## Other confirmed nonlocal-exit token leaks

### Finalizer owner on ERRFIN failure

`lj_gc2_finalizer_dispatch_one()` acquires `finalizer_active`, dequeues the
object, and calls `gc2_finalizer_dispatch_obj()`.  A user finalizer error is
normally caught and sent to ERRFIN.  But `gc2_errfin_vmevent_claimed()`
explicitly rethrows if the protected ERRFIN event itself fails (including a
fresh STOPREQ at that event boundary).  That throw bypasses
`lj_gc2_finalizer_leave()` in the outer dispatcher.  It can also bypass an
outer resume-claim release when that claim acquired an otherwise ownerless
state.  `finalizer_active` then permanently blocks finalizer draining and SWEEP.

### Weak-write counter around a throwing table retry

The internal VM/JIT weak store helper correctly calls the one-shot
`tab_trystoretv_cas_keyed_once()` while `weak_write_active` is nonzero.  Four C
API setters do not: `lua_settable()`, `lua_setfield()`, `lua_rawset()` and
`lua_rawseti()` call the exported looping `lj_tab_trystoretv_cas_keyed()` inside
their weak-write window.  On `LJ_TAB_STORE_CAS_CHANGED`, that helper calls
`lj_tab_store_wait_l()`, which performs an L-aware safepoint and may throw a
fresh STOPREQ.  The throw bypasses `lj_gc2_weak_write_end()`, leaving WEAK close
permanently blocked.  The analogous metatable store loop should be checked and
`lj_meta_tsettv_pair()` is confirmed to have the same looping-helper window; it
must be routed through the same one-shot primitive.

### GC-worker controller around JIT waits/events

`lj_gc2_workers_set_l()` owns `worker_control` before
`gc2_worker_start_count_locked_l()` calls `gc2_worker_prepare_traces_l()`.
That helper calls `lj_jit_token_acquire_wait()`, whose retry loop can throw a
fresh STOPREQ, and may call `lj_trace_flushall_hs()`, whose protected TRACE
event rethrows after releasing the JIT token.  Either exit bypasses
`gc2_worker_control_unlock()`.  Future worker start/stop calls then wait on a
controller which no longer exists.

## Required implementation design

The safe implementation should make the worker-owned graph engine nothrow and
give every failed publication a durable, allocation-free representation.
Protected cleanup remains useful at user callback boundaries, but must not be
the primary graph-work recovery mechanism.

### 1. Owner-local nothrow raw allocation

Add an accounting-aware helper with an explicit TG, for example
`lj_mem_new_nothrow_tg(global_State *g, TGState *tg, GCSize size)`.  Under the
internal arena policy it must allocate through `tg->allocd`, never through a
borrowed `lua_State`.  It returns NULL and never checks STOPREQ or unwinds.
Physical free can keep using pointer-owner routing.

Use it in `gc2_grey_grow()`, `gc2_weak_resize()` and
`gc2_finclaim_ensure()`.  Allocate all components first, free partial success on
failure, copy/initialize privately, then release-publish the complete tuple.
No pointer/capacity tuple may be half-published.

An inline initial grey ring in `GC2State` is preferable.  It guarantees a
minimum queue without any active-cycle allocation; larger replacement rings
remain an opportunistic performance optimization and can fail safely.

### 2. Durable failed-grey publication

Introduce a counted OOM/retry representation, not an assertion.  One practical
design which avoids an unbounded emergency queue is:

- on a failed direct grey push, set `LJ_GC_NEEDSCAN` on that exact traversable
  object and increment a type-appropriate orphan counter;
- retain the existing table and owner-thread counters, and add a counter for
  non-table container rescans;
- never clear a pending counter merely because SSB/grey queues are empty;
- when concrete queues drain, run a bounded, allocation-free scan of the GC
  ownership spine for NEEDSCAN containers and republish them into the now-empty
  inline ring;
- close MARK/WEAK/SWEEP only when all three classes of pending rescan are zero.

The ownership spine already contains all live GC bodies, so this failure-only
scan does not require an allocation or a new identity table.  Tables and
threads need their existing specialized clear/handoff rules.  Generic
non-table NEEDSCAN should be cleared and its counter decremented only after its
recovery publication is durable (or after a completed traversal, depending on
the selected queue-membership convention).

This design also requires removing the current MARK and SWEEP shortcuts which
declare a nonzero table pending count stale solely because grey/SSB are empty.
After OOM, queue emptiness is exactly when the pending bit is authoritative.

### 3. Transactional SSB consumption

Make `gc2_ssb_mark_one()` return a durable-publication result.  Do not clear an
SSB slot, decrement its count, retreat the active cursor, recycle a node, or
drop `ssb_consumer_active` until either:

- the object needs no traversal; or
- it is in grey; or
- its exact NEEDSCAN recovery bit/counter is published.

On an early return, republish `ssb_drain` before leaving the consumer scope.
Every exit from an SSB consumer must be ordinary C control flow, never a
longjmp.

### 4. Allocation-free weak fallback

Make vector and overflow-node failure publish a `weak_fullscan_pending` latch.
WEAK close then performs a bounded ownership-spine scan for marked tables whose
captured `LJ_GC_WEAK` flags are nonzero, processes them with the existing GC2
weak oracle, and clears the latch only after EOF plus the usual writer/mark
revalidation.  This path is cold (OOM/overflow only) and restores exact Lua
semantics without depending on the legacy weak-list bridge.

### 5. Transactional finalizer discovery

Make finalizer-node allocation return failure.  Reserve a node before claiming
or clearing FINREG state and before unlinking/finalizing the object.  If reserve
fails, leave the FINREG ownership intact, mark the object and finalizer value
live for this cycle, record a deferred-finalizer retry, and allow the cycle to
continue.  The next major cycle can retry.  Permanent metadata OOM may delay a
callback, but must neither abort nor reclaim its object.

### 6. Cleanup before rethrow at callback boundaries

Run `gc2_finalizer_dispatch_obj()` under an outer protected call owned by
`lj_gc2_finalizer_dispatch_one()`.  The protected return path must always:

1. leave `finalizer_active`;
2. drop the outer resume claim;
3. restore callback/hook/threshold state;
4. only then rethrow the captured error.

It is also reasonable to make `gc2_errfin_vmevent_claimed()` return an error
code instead of throwing, so all rethrows are centralized after token cleanup.

### 7. No L-aware waits inside weak-write or worker-control tokens

Export/use a one-shot keyed CAS helper in the C API weak-write windows.  End
the weak-write counter before any `lj_tab_store_wait_l()` retry.

For worker lifecycle, establish a nonthrowing lock order.  Acquire/flush the JIT
token before `worker_control`, or use an eventless try-only JIT preparation
after acquiring control.  Public TRACE events and STOP delivery must happen
after `worker_control` is released.  There must be no `lj_err_throw()`, Lua
allocation, user VM event, or L-aware wait under the controller token.

### 8. Snapshot states and formal nonblocking progress

Once owned work is nothrow, state 2 no longer leaks through OOM, but it remains
an opaque in-progress owner state.  `worker_active`, `hs_leader`,
`smr_reclaiming`, `mark_root_scanned=2`, and `weak_root_scanned=2` are not
formally lock-free if their owner can be indefinitely descheduled.  The final
lockless design must either make descriptors helpable or eliminate redundant
owner states.  For MARK/WEAK snapshot, `worker_active` already serializes the
production close path; a single published snapshot descriptor with idempotent
help is preferable to stacking another unhelpable boolean/tri-state owner.

## Deterministic validation required

Because custom `lua_Alloc` is temporarily ignored, allocator-OOM coverage must
use test-only fault injection at the GC2 raw-allocation helper.  A single hook
should distinguish grey grow, weak stack, weak ready, finclaim object,
finclaim value, dynamic SSB, weak overflow and finalizer node allocations.

Required tests:

1. **Cycle-start matrix** (`t-gc2-phase.c` or a new
   `t-gc2-owned-oom.c`): fail each cycle-start component, prove
   `worker_active==0`, `assist_active==0`, neither snapshot is 2, and a later
   retry completes a cycle.
2. **Large-fanout grey recovery** (`t-gc2-traverse.c`): use a table/prototype
   with more children than the inline ring, fail every attempted grow, and
   prove every reachable child is marked and survives through SWEEP.
3. **Detached SSB failure** (`t-gc2-worker-scheduler.c`): fail growth after a
   consumer detaches a multi-node chain.  Require `ssb_consumer_active==0`, no
   node/item loss, and eventual exact drain counts.
4. **WEAK state-2 failure** (`t-gc2-phase.c`): inject failure in the pre-root
   owned drain and prove state 2 and the worker token are not stranded; retry to
   successful weak completion.
5. **Assist failure** (`t-gc2-alloc-account.c`): force a full ring past the hard
   limit and require `tg->gc_assist`, `assist_active`, and `worker_active` all
   return to zero while deferred work remains recoverable.
6. **Weak vector/overflow OOM** (`t-gc2-traverse.c`): fail both vector and every
   overflow node, pass NULL bridge head as production does, and verify weak-key,
   weak-value and all-weak clearing exactly.
7. **Finalizer node OOM** (`t-m8-finalizer-state.c`): prove the candidate stays
   live and FINREG-owned, then remove the fault and prove exactly-once callback
   delivery.
8. **ERRFIN throw cleanup** (`t-m8-finalizer-state.c`): make the protected
   ERRFIN path or the test dispatch callback throw; catch it outside and assert
   `finalizer_active==0`, owner tid zero, any acquired state claim released, and
   a second finalizer remains dispatchable.
9. **Weak CAS/STOPREQ** (`t-tab-cas-store.c` plus a deterministic forced-CHANGED
   hook): arm fresh STOPREQ during a weak C-API store retry, catch the error, and
   assert `weak_write_active==0` before completing WEAK.
10. **Worker-control throw** (`t-gc2-worker-scheduler.c`): hold the JIT token or
    install a failing TRACE event, interrupt `threading.gcworkers()`, and prove
    a subsequent start/stop operation can acquire `worker_control`.
11. **Owner-local allocator proof**: pause a background grow while the main TG
    allocates and assert the side vector's allocation owner is the worker TG,
    then run TSAN/ASan stress.
12. **Preemption/help tests**: pause after every close/snapshot owner
    publication and require another worker to complete or safely supersede it.
    These tests are needed for the final nonblocking claim even after all OOM
    paths are nothrow.

Every fault case must run with assertions enabled and disabled.  The release
build is essential because the current `lj_assertG(pushed, ...)` sites otherwise
hide silent work loss behind a test abort.

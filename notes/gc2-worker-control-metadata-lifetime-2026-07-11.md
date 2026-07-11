# GC2 worker-control metadata lifetime (2026-07-11)

## Failure

Repeated JIT-enabled runs of `tests/t-gc-workers.lua` rarely crashed while
foreign `threading.spawn()` controllers repeatedly changed `gcworkers()`.
JIT-off runs were clean enough to make this initially look like a trace bug.
Two symbolized GDB cores instead stopped in:

```
gc2_worker_reclaim_retired_tgs
  -> lj_tg_fini_thread
    -> lj_str_flush_num_credit
```

The retired-list head named a 16-byte cell whose bytes had become an
`LJArenaFreeRun` (`start == cell`, `len == 2`). Thus the list retained a node
that the main plain arena had already made reusable.

## Root cause

Parked-worker control used `mainthread_acq(g)` as the allocation context for
all of these runtime-only records:

- `LJThr`;
- `TGState`;
- a separate `GC2WorkerTGRetire` list node.

`gcworkers()` may be called by an attached foreign Lua thread. Passing the
shared main `lua_State` to `lj_mem_new*()` in that thread selects the main TG's
owner-private bump/bin allocator. `worker_control` serializes worker-pool
controllers, but it does not stop the real main thread or its JIT compiler from
allocating concurrently. The foreign controller therefore raced the main arena
owner. JIT compilation supplied the timing and allocation pressure; generated
code was not on either crashing stack.

The matching stop paths also called `lj_mem_free()` for worker metadata from
whichever controller happened to stop the pool. Remote object frees are
supported, but remotely driving allocation and lifecycle bookkeeping through
the main `lua_State` is not.

## Fix

Worker retirement is now allocation-free:

- each worker `TGState` contains an atomic `worker_retire_next` link;
- `gc2.worker_tg_retired` is a list of the quiescent worker TGs themselves;
- registry unlink remains the condition for physical TG finalization;
- reclaim clears the embedded link before finalizing and freeing the TG;
- the separately allocated retirement-node type and its free path are gone.

`LJThr` and worker `TGState` storage now use checked `malloc()` and matching
`free()` on every normal, partial-start, thread-create-failure, retirement, and
shutdown path. OOM returns worker-start failure; it no longer raises/longjmps
through a remotely borrowed main `lua_State`.

This is an intentional control-plane boundary. Reconfiguring `gcworkers()`
already creates/joins OS threads, takes the worker-control gate, and may wait;
it is an explicit blocking definition/control operation. Steady-state GC worker
draining remains allocation-free and non-blocking. The external worker records
are not Lua heap objects and are deliberately absent from Lua allocator byte
accounting. Custom `lua_Alloc` is temporarily ignored project-wide, as recorded
elsewhere; restoring that API later must not reintroduce foreign access to an
owner-private allocator.

Worker stop now treats a successful OS join as the quiescence linearization
point. A failed `pthread_join()`/`WaitForSingleObject()` retains the `LJThr`, TG
slot, nonzero worker count, and STOP publication so a later controller can
retry. Explicit reconfiguration reports failure. Terminal close fails hard
rather than reclaiming storage which an unjoined worker might still access.

## Cross-universe terminal lifetime

One OS thread may naturally operate on two independent Lua universes while raw
TLS remains bound to the first. The ordinary dead-TG reclaimer intentionally
requires raw TLS to name the requested universe's main TG, so stopped workers
and previously joined Lua thread TGs in the second universe could otherwise
remain registered through `lua_close()`.

`lj_tg_reclaim_dead_terminal()` is a shutdown-only form of the same serialized
scan. It bypasses only the raw-TLS identity condition and still requires
shutdown admission to be closed, exactly the main TG counted live, no pending
handshake, no live or entering mutator, no GC worker, and successful acquisition
and recheck of the single-writer CAS gate.

Close invokes it before `freeall`, again after terminal SSB discard can drop a
dead TG's final embedded-node pin, and once more in GC2 finalization. Unflagged
worker TG storage stays owned by the embedded retire list after registry unlink;
heap and deferred Lua-allocated TGs retain their matched destruction paths.

The adjacent audit also fixed two cross-universe raw-TLS assumptions: pending
root flushing excludes a TLS TG whose `gl` names another universe, and per-TG
PRNG derivation accepts a TLS parent only from the requested universe. New GC
workers derive their PRNG stream after receiving a unique tid.

## Regression coverage

`tests/t-gc2-worker-scheduler.c` holds an extra TG in the registry while two
workers stop, forcing deferred worker-TG retirement. It asserts that:

- the retirement head is exactly one stopped worker TG;
- its embedded link names exactly the other stopped worker TG;
- terminal registry reclamation clears the retired head;
- starting a replacement pool does not change Lua allocator byte accounting.

It also forces one `pthread_join()` failure and proves the thread/TG records are
retained until a successful retry. A second-universe fixture stops workers while
TLS remains in the first universe, pins a deferred Lua TG across the first
terminal pass, verifies pending-root isolation, and closes the second universe
without changing TLS.

The repeated JIT-on/JIT-off worker-control stress is also run after the focused
fixture to cover the original timing-sensitive failure.

Validation on the fixed binary:

- default incremental build: passed;
- focused `t-gc2-worker-scheduler` (including embedded-chain and accounting
  assertions): passed 100/100 after the full integration patch;
- `t-gc-workers.lua`, JIT enabled: 5,000/5,000 fresh-process runs passed;
- `t-gc-workers.lua`, `-joff`: 5,000/5,000 fresh-process runs passed.

The final integrated binary, including terminal and join-failure hardening,
also passed another 1,000/1,000 JIT-on and 1,000/1,000 JIT-off worker runs. The
registered `m3_gc2_worker_scheduler` case passed with both JIT modes.

For comparison, the intermediate build which embedded the retirement link but
still allocated `LJThr` and worker `TGState` through the main allocator crashed
at JIT-on iteration 585. This is why the fix covers the complete worker-control
metadata lifetime rather than only the first corrupt node observed in GDB.

## Remaining allocator-transfer debt

Runtime dead-TG transfer is still tactical. Each per-TG huge-object table has a
fixed capacity, and transferring the aggregate live huge mappings into the main
TG can fail if that destination fills. The ordinary reclaimer must then retain
the dead TG. Terminal shutdown still needs a post-`freeall` orphan-allocator
drain which can finalize any such retained allocator without first fitting its
now-dead mappings into the main table. The planned global/orphan allocator
ownership design must remove this capacity-dependent lifetime edge.

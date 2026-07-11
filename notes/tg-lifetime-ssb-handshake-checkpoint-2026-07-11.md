# TG lifetime, logical-SSB, and prior-handshake checkpoint (2026-07-11)

Status: implemented checkpoint with known tactical lifetime debt. This is not a
claim that TG reclamation, GC2, JIT, or the multithreaded runtime is complete or
release-ready. The plan files are unchanged.

## Attach, detach, and physical TG lifetime

The immediate lifetime bug was a dead `TGState` becoming physically
reclaimable before a foreign detach/failure epilogue had finished using its
`lua_State`, `global_State`, TLS, or owner lookup. A second reclaimer could also
walk and mutate the same registry concurrently.

The checkpoint establishes the following protocol:

1. `threading_entering_begin()` acquire/release-increments `mt_entering` before
   an admitted attach reads the TG registry. It also participates in a
   two-sided try protocol with `gc2.tg_reclaiming`: an entrant keeps its
   admission reservation while the writer gate is set, and a writer which sees
   an entrant after publishing its gate abandons rather than waiting.
2. Foreign attach retains its entering reservation across the protected attach
   transaction. Once `threading_gc_enter_counted()` publishes `mt_live`, a
   failed transaction retains both lifetime reservations through cleanup.
   `threading_attach_cleanup()` clears TLS and owner hints, releases the state
   claim, and disposes of an unregistered private TG before releasing those
   reservations. The final `mt_live` decrement is the final access to `g` on
   that path. A protected attach error is reported as attach failure rather
   than rethrown after the final VM-lifetime reservation has been released.
3. A TG which reached registry publication is never reclaimed inline by its
   foreign cleanup. Detach first acknowledges any owned request, terminally
   publishes its SSB, flushes allocation/string accounting, and clears every
   remotely sampled owner field. Same-thread TLS is cleared before a release
   fence and the `TGF_DEAD` publication; only then is `n_threads` decremented.
   The detacher's later state-release work remains covered by `mt_live`.
4. `lj_tg_reclaim_dead()` is presently a nonwaiting try-writer. It requires the
   runtime-main TLS owner, one live TG, no pending handshake, no `mt_entering`
   or `mt_live` users, and no GC2 workers. It then CAS-publishes
   `gc2.tg_reclaiming` and repeats those checks before walking or unlinking.
   This serializes current writers and prevents the admitted attach/detach
   epilogues from crossing physical unlink.
5. A dead TG with a published embedded SSB node remains pinned by `ssb_refs`.
   Reclamation transfers its allocator state before unlink and frees storage
   only after the last embedded publication is recycled. Heap foreign-attach
   TGs use `free`; a `threading.thread` TG whose userdata relinquished its
   storage while pinned carries `TGF_LUA_ALLOC|TGF_DEFER_FREE` and is finalized
   through `lj_mem_freet` by the reclaimer.
6. Native live-root nodes are tombstoned by threading shutdown but are not
   physically freed until `close_state()` has stopped and joined the GC2 worker
   pool. This keeps a collector's scoped raw-node snapshot valid through its
   read section.

The relevant implementation surfaces are `src/lib_threading.c`,
`src/lj_tg.c`, `src/lj_tg.h`, `src/lj_state.c`, and the `gc2.tg_reclaiming`
accessors in `src/lj_obj.h`.

## Terminal and logical-owner SSB flushes

`lj_gc2_flush_ssb_detach()` is a terminal, allocation-free rotation. It
publishes a nonempty active node without allocating or installing a replacement
buffer, then clears the TG's active cursor. The embedded-node publication takes
an `ssb_refs` pin, so a worker may drain it after `TGF_DEAD` without accessing
freed TG storage. Recycle drops the pin only after its final owner access and
does not return a node to a dead TG's free list.

A separate multistate bug came from confusing raw OS-thread TLS with the
`lua_State` which logically initiated collection. For example, code running in
state `L1` may create an independent state `L2` and invoke public GC on `L2`
while TLS correctly remains bound to `L1`. A GC2 drain which inferred its local
SSB only from raw TLS could leave `L2`'s active suffix unpublished and prevent a
forced collection from reaching an exact empty frontier.

The bounded drain path now carries an explicit logical TG. Public collection
and fixpoint work pass `L2TG(L)` through `gc2_worker_drain_logical()` and
`gc2_worker_drain_budget()` into `gc2_worker_drain_inner()`. It rotates that
active SSB only when the supplied TG belongs to the requested `global_State`
and is not dead. Background workers still supply their actual TLS TG, and no
unproved fallback owner is invented in the inner drain.

`test_multistate_public_api_gc()` in `tests/t-safepoint-handshake.c` preserves
`L1` in TLS, creates `L2`, runs both Lua and public-API full collections on
`L2`, and requires `L2` to finish in IDLE with an empty SSB and reset active
cursor while TLS still names `L1`.

## Finishing a previously published handshake

A synchronous handshake could begin while an earlier asynchronous request
still owned counted `reqmask` slots. The important observed shape was an async
`STOPREQ` arriving between bytecodes and an allocation-triggered synchronous
GC handshake. Replacing `hs_pending` with the new leader sentinel lost the old
slot: the later acknowledgement could then subtract from zero and underflow the
pending count.

After leader serialization and before publishing new actions or the new
sentinel, `lj_safepoint_handshake()` now calls
`safepoint_finish_prior_epoch()`. It acknowledges locally actionable work and
uses the existing futex boundary until the prior `hs_pending` reaches zero.
Only then may the new epoch install its sentinel. Applying a prior `STOPREQ` at
this point publishes its sticky/fresh state; the ordinary VM/native exit
boundary still owns the user-visible interruption.

## Validation at this checkpoint

- A clean/default build completed successfully.
- The new `tests/t-threading-lifecycle.c` passed 50/50 direct repetitions and
  passed through the registered `m4_threading_lifecycle` suite case. Its linker
  wrappers deterministically hold detach at state release, force failed attach
  to overlap `lua_close`, and attempt a competing foreign reclaimer. It also
  verifies that an embedded SSB pin defers physical TG reclamation.
- The full direct `tests/t-safepoint-handshake.c` fixture passed after the
  prior-epoch and logical-owner changes, including the multistate public-GC
  assertions.
- `tests/t-gc2-phase.c`, `tests/t-gc2-alloc-account.c`, and
  `tests/t-gc2-markbits.c` each passed 20/20. `tests/t-gc2-traverse.c` passed
  100/100 pinned to one CPU and 100/100 under normal scheduling after its
  replacement-cycle fixture was made tolerant of a second legitimate child
  result-root abort.
- The arena terminal model passed 5/5, arena sweep and runtime arena GC sweep
  passed 20/20 each, the full safepoint fixture passed 20/20, and the final
  worker scheduler passed 100/100.
- The rare JIT-on worker crash was diagnosed from symbolized cores as foreign
  worker-control allocation racing the main TG allocator. Retirement is now
  allocation-free and worker-only control records use matched system
  allocation. The fixed crash binary passed 5,000/5,000 JIT-on and 5,000/5,000
  JIT-off runs; the final integrated binary passed another 1,000/1,000 in each
  mode. See `gc2-worker-control-metadata-lifetime-2026-07-11.md`.
- The registered `m3_gc2_worker_scheduler` and `m4_threading_lifecycle` cases
  both passed.

## Tactical debt and required replacement

These changes close the reproduced epilogue/reclaimer races. They do not form a
general physical-lifetime proof for every TG registry reader.

### First-use attach versus close remains outside the GG lifetime

`lj_threading_attach(L)` must dereference `L` to obtain `G(L)` before it can
increment `g->mt_entering`. If the very first foreign use races a concurrent
`lua_close()` before that admission point, an in-GG counter cannot protect the
GG containing the counter. The embedding API ultimately needs an external VM
lifetime handle/lease (or an equivalent owner contract whose storage outlives
GG) so a caller can prove `L` and `g` live before entering this protocol.

### Migrated close can strand a raw TLS pointer

Cross-universe same-thread close is valid and covered: TLS may belong to `L1`
while independent `L2` is collected and closed. The unresolved case is a VM
created/bound on OS thread A and closed on OS thread B. Thread A can retain a raw
TLS pointer to the embedded main TG after B frees GG; later profiling/SIGPROF
code may follow `tg->gl` through freed storage. Clearing only the closing
thread's TLS cannot repair another thread's slot. TLS ownership needs a retained
or generation-checked handle, or scoped universe bindings whose invalidation is
observable without dereferencing freed TG/GG storage.

### The current registry writer gate is deliberately coarse

The reclaimer currently requires the runtime-main TLS writer and suppresses
physical reclaim whenever `n_workers`, `mt_entering`, or `mt_live` is nonzero.
In particular, a started but parked GC2 pool postpones ordinary dead-TG reclaim.
This is a tactical safety gate: it can retain dead nodes, concentrates work on
the main owner, and does not authorize unleased direct registry readers. It is
not the intended steady-state lockless reclamation design.

Terminal shutdown now has a separate strictly quiescent scan which permits
cross-universe close without rebinding raw TLS. It bypasses only the main-TLS
identity check and does not weaken the ordinary runtime gate. Fixed-capacity
huge-table transfer can still retain a dead TG; a post-`freeall`
orphan-allocator drain remains required, as documented in the worker-control
lifetime note.

### Recommended bounded read-lease reclaimer

Replace coarse reader suppression with an explicit TG-registry read lease:

1. A reader acquires a generation/epoch lease, rechecks writer admission, and
   either obtains a stable snapshot or returns/retries without waiting.
2. A writer closes new lease admission with a CAS, unlinks only a bounded number
   of dead nodes, and moves them to a retired list. It must not wait for readers
   while holding up mutators.
3. Physical allocator transfer/finalization occurs only after a later bounded
   grace pass proves every pre-unlink lease has expired and all embedded SSB
   pins are zero.
4. The writer then reopens admission and wakes ordinary scheduler retry paths.
   Budgets must cap registry nodes, transferred arenas, and finalizers so a long
   dead list cannot turn one GC step into an unbounded pause.
5. TLS/profile readers use the generation-checked retained handle described
   above; a GG-local registry lease alone cannot solve the pre-admission
   first-use race or a raw pointer stranded on another OS thread.

Until that replacement and the owner/global-root split are complete, the
current gates should be treated as correctness scaffolding, not evidence of a
fully nonblocking runtime.

# 05. The Concurrent Garbage Collector (lj_gc2)

The collector is an **on-the-fly, parallel, non-moving mark & sweep**:
grey-stack Dijkstra marking with a phase-gated insertion store barrier,
black allocation during marking, soft-handshake root rescans iterated to a
fixpoint, then a handshaked flip into lazy/parallel bitmap sweeping. No
thread ever stops another thread. Lineage: heap & bitmaps from Pall's
LuaJIT-3.0 design (04); marking protocol from the DLG/FUGC family; the
write-barrier-free stack treatment is paid for by the rescan fixpoint.

New files: `lj_gc2.h`, `lj_gc2.c`, `lj_safepoint.h/.c`. The legacy
`lj_gc.c` state machine (GCSpause..GCSfinalize, lj_gc.h:12–14, lj_gc.c:657)
is not a production build path after `lj_gc2` lands; keep reference pieces
only where this plan explicitly calls for a debug oracle.

## 5.1 Why this protocol (one paragraph you must internalize)

A Dijkstra insertion barrier ("mark what you store") keeps every reference
that *enters* the heap during marking alive. The only references it can
miss are ones that exist purely in thread stacks/registers. Those cannot be
created from nothing: a stack ref was loaded from the heap (where the
object was reachable at load time — and stays marked-or-grey because heap
edges only get *added* under the barrier and removal cannot unmark) or from
a new object (born black, I-3). So repeatedly re-scanning stacks must
converge: each round can only discover objects that were live at round
start, the marked-set grows monotonically and is bounded. When a full round
of (rescan all roots + drain all grey work) marks nothing new, every
reachable object is black — the fixpoint. That argument is the whole
correctness story; every mechanism below just implements it efficiently.

## 5.2 Phases

```
P_IDLE → P_MARK → P_WEAK → P_SWEEP → P_IDLE
```
- transitions are made by the **GC leader** (worker 0) and become visible
  to mutators only through handshakes;
- mutators read at most two phase-derived flags on hot paths:
  `TG.mark_active` (store barrier on/off) and `TG.alloc.alloc_black`
  (allocation color) — both are *per-thread mirrors written by the thread
  itself during handshake acks*, so hot paths never load shared phase state.
  Attach catch-up now mirrors the current bridge phase explicitly: `P_MARK` and
  `P_WEAK` adopt barrier-on/black allocation, `P_SWEEP` adopts barrier-off/black
  allocation for the current black-through-cycle simplification, and `P_IDLE`
  adopts barrier-off/white allocation. The previous attach bridge only treated
  `P_MARK` as active.

## 5.3 State (struct GC2State, in global_State)

```c
typedef struct GC2State {
  uint32_t phase;              /* P_*; written by leader               */
  uint32_t cycle;              /* monotonically increasing cycle id    */
  uint64_t hs_epoch;           /* handshake generation (see §5.4)      */
  uint32_t hs_pending;         /* acks outstanding (atomic countdown)  */
  uint32_t hs_actions;         /* HS_* bits of current handshake       */
  uint32_t hs_leader;          /* handshake leader owner token         */
  TGState *tg_list;            /* lock-free singly-linked list of TGs  */
  uint32_t n_threads;
  /* marking */
  GreyDeque *wdeq;             /* one Chase-Lev deque per GC worker    */
  uint32_t  n_workers;
  _Alignas(64) uint64_t marks_this_round;   /* fixpoint detector       */
  GCRef     weaklist;          /* lock-free stack of weak tables found */
  GCRef     finqueue_head;     /* MPSC: objects needing finalization   */
  DeferList deferred[2];       /* grace-period reclamation (§5.9)      */
  /* pacing */
  uint64_t alloc_since_trigger, trigger_bytes, hard_bytes;
  uint32_t gcpause_pct, assist_shift;     /* tunables (lua_gc mapped)  */
  uint8_t  generational;       /* §5.12 */
  uint8_t  torture;            /* §5.13 */
  PendingBC pendingbc;         /* JIT flush coordination (08 §8.7)     */
} GC2State;
```

## 5.4 Soft handshakes (lj_safepoint.c)

### 5.4.1 Thread registry
TG blocks form a lock-free list: `lj_tg_attach` CAS-prepends to
`gc2.tg_list`; detach marks `TG.tg_flags|=TGF_DEAD` (physical unlink + free
deferred to the leader between cycles — the list is only ever walked by the
leader and workers).

### 5.4.2 Protocol
```
leader:  hs_actions = bits; hs_pending = live_thread_count;
         hs_epoch++ (release);
         for each TG: la_store32_rel(&TG->reqmask, bits),
                      la_store32_rel(&TG->poll, 1);
         wake any parked mutators (futex on their park words: channels,
         join, sleep all park on TG-adjacent words and re-check poll);
         for each TG currently in_native: perform its actions remotely
         (§5.4.3) and ack on its behalf (CAS hs_epoch_ack);
         wait: futex on hs_pending → 0 (workers may also steal: they
         process actions for threads that flip in_native meanwhile).
mutator (at any poll site, via ->vm_safepoint → lj_safepoint_ack(L)):
         acts = la_xchg32_acqrel(&TG->reqmask, 0); TG->poll = 0;
         perform each HS_* action (below);
         TG->hs_epoch_ack = hs_epoch; la_sub32(&hs_pending,1);
         futex_wake(&hs_pending) if it hit 0.
```
The only waiting party is the GC leader; mutators never wait (lock-free per
§2.2). A mutator that never polls would stall *the GC only*; pacing's hard
limit then throttles allocation on that thread alone (§5.11) — liveness of
other mutators is unaffected.

HS action bits: `HS_ENABLE_BARRIER, HS_DISABLE_BARRIER, HS_ALLOC_BLACK,
HS_ALLOC_WHITE, HS_SCAN_ROOTS, HS_FLUSH_SSB, HS_RESET_ALLOC (move owned→
needsweep, drop bins/bump), HS_EXIT_TRACES (just exits: the poll in trace
code jumps to a side exit, so by ack time the thread is in the interpreter),
HS_REDISPATCH (03 §3.6), HS_FLUSHJ (08 §8.7), HS_STOPREQ (09 §9.6).`

### 5.4.3 Native state
Before any potentially-blocking C (FFI call F-class, lua_CFunction that may
block, channel park, OS sleep): `lj_native_enter(tg)` = publish L->top &
frame (already maintained), `la_store8_rel(&tg->in_native,1)`. On return:
`in_native=0 (rlx)`, then if `poll` → ack normally. While in_native, the GC
may execute that thread's HS actions itself: scanning its (quiescent) Lua
stack is safe because the thread, by contract, does not touch Lua state
while in native (FFI callbacks re-enter via lj_native_leave first; 11
§11.5). Races at the boundary are resolved by `hs_epoch_ack` CAS: whoever
CASes old→new performs/owns the ack; the loser does nothing.

Current bridge note: native exits may also be shutdown delivery points.
`os.execute`, `os.remove`, `os.rename`, POSIX `os.tmpname`/`mkstemp`, and the
`lib_io` `io.tmpfile()`, `fflush`, and `file:seek()` native calls now feed their
`lj_native_leave(L)` result into `lj_safepoint_checkstop()`, so a STOPREQ
acknowledged by the native leave is raised before those functions build their
normal Lua results. The POSIX `os.tmpname` helper unlinks the just-created
temporary file before raising when Lua will not receive the filename, and
`io.tmpfile()` closes the just-created handle before raising. The broader
native-exit audit for IO, FFI, callbacks, and other wrappers remains separate
follow-up work.

## 5.5 Mark state & colors

Mark = bit in the arena `mark` bitmap (or huge-table flag). Set with
`la_bit_test_and_set64` (relaxed fetch_or; the returned previous bit
deduplicates grey pushes). "Black vs grey" is positional: grey = mark bit
set AND sitting in some grey container; black = marked and drained.
Allocation color: during P_MARK (and until each arena is swept in P_SWEEP),
allocators set the mark bit at birth (`alloc_black`, 04 §4.4) — newly
allocated objects are never scanned and never collected this cycle. During
P_SWEEP, allocation from an already-swept arena is white (its bits were
just rewritten), from an unswept arena black — distinguished per-arena by
`sweep_epoch == cycle`, mirrored into the bump window when adopted (FUGC
does exactly this).

## 5.6 Grey work

### 5.6.1 mark_obj — the one true marking entry
```c
static LJ_AINLINE void gc2_mark(TGState *tg, GCobj *o) {
  GCArena *a = arena_of(o);
  if (LJ_UNLIKELY(a->hdr.flags & LJ_HUGE_MAGIC)) {
    huge_mark(owner_tg(a->hdr.owner_tid), o); return;
  }
  uint32_t c = cell_of(o);
  if (la_bit_test_and_set64(&a->mark[c>>6], c&63)) return; /* already */
  if (!(a->hdr.flags & AF_TRAVERSABLE)) return;  /* strings etc: done  */
  grey_push(tg, o);
}
```
Non-traversable arenas make string marking bitmap-only (no push, no object
touch) — directly Pall's segregation payoff.
Current bridge note: arena mark/ismarked helpers resolve `GCAhdr.owner_tid`
for huge allocations before consulting a huge table, and mark-begin clears
arena/huge marks across the TG list. This preserves the original owner-table
design while the full worker sweep protocol is still staged.

### 5.6.2 Mutator side: the SSB
`grey_push` from a mutator appends to `TG.ssb` (array of GCRef, 1 KB).
On overflow or at `HS_FLUSH_SSB`: publish the full buffer node onto a
global MPSC stack (`la_xchgptr` head swap) and grab a fresh buffer from a
local pool. Workers consume published buffers into their deques. Mutators
**never trace** (except assists §5.11); the SSB keeps barrier slow-path
cost at "store + bump".

### 5.6.3 Worker side: Chase–Lev deques + arena affinity
Each worker owns a Chase-Lev deque of GCobj* (classic owner-push/pop bottom,
thieves steal top with CAS; implement verbatim from the paper/known code —
~120 lines; goes in lj_gc2.c with the standard acquire/release placement;
TSAN-clean test in 13 §13.6.2). Workers drain: pop → `gc2_traverse(o)` →
children via gc2_mark (worker variant pushes to own deque). Steal when
empty; then consume SSB stack; then declare locally idle. Optional locality
upgrade (M9): route pushes through per-arena grey stacks with an arena
priority queue as in Pall's doc; the deque design is the correctness
baseline and may already be sufficient.

Current bridge note: `lj_gc2_worker_drain()` now provides a bounded non-owner
worker entrypoint that converts published SSB entries to grey work, steals
from the Chase-Lev top side, and traverses stolen objects. The full worker
pool, grow-safe per-worker deque ownership, idle declaration, and scheduling
remain the original target above. The current global grey deque has a temporary
single-worker ownership token (`worker_active`) around this drain surface, plus
busy/idle telemetry, so overlapping helper calls do not both act as the staged
worker owner. `lj_gc2_worker_drain()` returns total progress, including
leaf-only SSB conversions that do not traverse a grey object, so future
idle/fixpoint loops do not need to infer progress from telemetry counters.
The temporary `_progress()` compatibility alias has been removed. During
`P_WEAK`, any remaining worker budget can also advance `lj_gc2_weak_drain()`
through the published weak snapshot, with `worker_weak_drained` attributing
that bounded work; the full scheduler-owned weak drain remains staged. During
`P_SWEEP`, the same temporary worker owner can spend its budget on bounded
traversable arena sweep batches through `lj_gc2_sweep_owner_progress()`, after
the legacy sweep boundary prepares the arena lists and restores plain arenas.
This keeps the previous boundary-lazy sweep bridge but removes its direct sweep
loop from `lj_gc.c`.

Current parked-worker delta: the original target remains the full worker pool
with per-worker Chase-Lev ownership, grow-safe deque migration, steal/idle
declaration, and scheduler-owned phase transitions described above. The current
implementation adds an explicit, opt-in `lj_gc2_worker_start/stop/wake`
lifecycle for a capped two-worker parked pool over the existing bounded
`lj_gc2_worker_drain()` surface. It does not auto-start workers and does not add
an `LJ_MT`/`LUAJIT_THREADSAFE` lock gate. Phase transitions and mutator SSB
publication now wake started parked workers, with
`worker_wakes`, `worker_parks`, and `worker_async_progress` recording the
bridge behavior until the full scheduler replaces the single-owner token.
Parked workers now attach real no-Lua-stack TGs and use TG TLS, so safepoint
self-identification no longer falls back to the main TG. Their TG storage is
kept alive until the lockless TG registry has reclaimed the dead node. They can
be remotely acknowledged as native TGs with no current `lua_State`.
`collectgarbage("workers", N)` exposes that staged lifecycle to Lua: missing
`N` queries the current worker count, `N <= 0` stops the parked workers, and any
positive `N` starts the capped parked pool while reporting the previous count.
Current lifecycle guard: parked worker start/stop/set operations are serialized
by `GC2State.worker_control`, so racy Lua calls to
`collectgarbage("workers", N)` cannot concurrently mutate worker thread/TG
slots. Waiters park as native TGs while waiting for the control word. Dead
worker TGs that are still registry-visible are moved to a retired list and
freed only after TG reclamation unlinks them, so rapid control churn can reuse
worker slots without violating TG registry lifetime.

### 5.6.4 gc2_traverse — per-type tracing
Port the existing traversal logic, replacing color plumbing:
- `gc_traverse_tab` (lj_gc.c:174): mark metatable, then **load the current
  gen headers with acquire** and walk that snapshot's array+nodes (a racing
  resize is fine: the old gen's contents were copied with barriered stores,
  and the new gen header store is a heap edge under the barrier ⇒ either
  snapshot is safe to trace). Weak-mode tables: read mode from gcflags
  cache; if weak, push onto `gc2.weaklist` (lock-free stack via gcw link)
  and trace only the strong half (key/val per mode) — semantics of
  gc_mayclear/gc_clearweak (lj_gc.c:457–501) are preserved in §5.10.
- `gc_traverse_func/proto/thread/trace` (lj_gc.c:225/280/309/256): ports
  with cell-upvalue simplification (uvval is always &uv->tv) and the thread
  rule of §5.7.2. Trace traversal: a GCtrace is immutable post-publish
  (I-6); walk its IR constants exactly as gc_traverse_trace does.
- cdata: only finalizer-registered cdata are traversable via the miscmap
  entry; payload pointers are not traced (FFI memory is unmanaged), so
  cdata arenas are non-traversable. (11 §11.4.)

## 5.7 Roots & the fixpoint loop

### 5.7.1 The leader's mark phase
```
P_MARK entry:
  cycle++; clear marks NOT needed (the sweep flip already left mark bits
  correct: after major sweep mark'=block^mark makes live objects white);
  handshake H_on = {ENABLE_BARRIER, ALLOC_BLACK, SCAN_ROOTS, FLUSH_SSB,
                    RESET_ALLOC? no — alloc reset is sweep-side}
  loop rounds:
    marks_this_round = 0
    workers drain everything (deques, SSB stack, suspended-thread scans §5.7.2,
      global roots §5.7.4)
    handshake H_round = {SCAN_ROOTS, FLUSH_SSB}
    workers drain again to empty
    if marks_this_round == 0 and all deques+SSB empty: fixpoint reached
  → P_WEAK
```
`marks_this_round`: every successful first-time bit set adds 1 to a
per-TG/worker counter, flushed to the global at handshake/idle —
cheap, exact-enough monotone detector. Two empty rounds are required by
the detector only if counters are flushed lazily; with flush-at-round-end
one zero round suffices (prove in code comment; the model in
aux/nbtab_model.c is unrelated — write a 30-line unit test for the
detector in lj_gc2_test.c, 13 §13.5).

Current bridge note: `lj_gc2_fixpoint_round()` is the first bounded
leader-side implementation of one `P_MARK` §5.7.1 round. It resets
`marks_this_round` with an atomic exchange, drains current published work
through the worker-drain surface while counting leaf-only SSB conversion as
progress, handshakes `{SCAN_ROOTS, FLUSH_SSB}`, drains again, and reports the
zero-mark/empty-work predicate. `lj_gc2_fixpoint_run()` repeats that bounded
round and the legacy atomic bridge now calls it before paranoia/weak clearing,
so current cycles exercise the detector. Final legacy atomic mark completion
now routes through `lj_gc2_mark_complete()`, and `lj_gc2_mark_to_weak()` owns
the `MARK -> WEAK` publication telemetry. The original full loop above remains
the target: a scheduler-owned pool, per-worker idle declaration, and independent
leader ownership of the `P_WEAK` transition are still follow-up work. The current
empty-work predicate includes the global grey deque, published SSB stack, and
each live TG's active SSB cursor, read through acquire loads while walking the
shared TG list. Active SSB producers publish cursor advances with release
stores after writing the slot; active SSB drains release-publish cursor
retreats only after marking/enqueuing the popped slot. The original final
mark-completion bridge could return a miss while another worker held the
temporary `worker_active` drain token; `lj_gc2_mark_complete()` now waits for
that peer drain to finish and retries before the legacy bridge can publish
`P_WEAK`, recording `mark_complete_peer_waits` telemetry. The `MARK -> WEAK`
and `WEAK -> SWEEP` publication helpers are now CAS-gated from their expected
source phase, so stale helper calls cannot force an idle or wrong-phase
collector into a later phase or run the sweep-entry handshake.

### 5.7.2 Thread stacks
`HS_SCAN_ROOTS` (mutator, at ack): for its *running* L (and the chain of
parents if inside coroutine.resume — walk `L->cframe`/frame links exactly
like gc_traverse_frames lj_gc.c:292): mark every TValue in
`stack..top`+frame extras, mark L itself, its env, legacy openupval list
values. This is precise and fast (linear TValue scan; ~1 GB/s ⇒ a 1 MB
stack costs ~1 ms, typical stacks ≪ that). Current bridge implementation
loads each running stack slot into a local snapshot before marking. GC2 worker
traversal now claims unowned suspended coroutine stacks with
`lj_state_gcscan_claim()` before scanning and records busy deferrals when a
running owner holds `thr_owner`. Owner safepoint scans now publish the thread's
`scan_epoch`; if the owning TG's current `cur_L` is the thread and that epoch
matches the current GC2 cycle, the worker records owner coverage. Otherwise GC2
requeues the thread for a later claim or owner-scan attempt so the fixpoint
predicate does not silently go empty. Busy stacks with a known live owner now
also set `LJ_GC_NEEDSCAN`; the owning TG's next root scan walks owned pending
coroutine stacks, publishes their `scan_epoch`, and lets requeued worker items
observe same-cycle owner coverage instead of requeueing forever. The
owner-coverage predicate now also records the owning TG's
`stack_dirty_epoch` in `lua_State.scan_dirty_epoch` at scan time. Worker
traversal only accepts a same-cycle owner scan when that dirty stamp still
matches the owner, so a resume/yield/state handoff after the root scan forces
a fresh `LJ_GC_NEEDSCAN` owner pass instead of silently skipping a changed
stack. C-side `lj_state_release()` and the x64 coroutine fast release path
advance the owner dirty stamp; successful worker claims publish a scan stamp
and clear pending owner-scan state. The broader original target remains to
expand dirty maintenance across every future foreign-state operation as those
APIs land.
Suspended coroutines: workers traverse them as ordinary heap objects, but
must hold the claim: `CAS th->thr_owner 0→GCSCAN`; on failure (running),
set `th->gcflags|=GCF_NEEDSCAN` — the owner's next HS_SCAN_ROOTS scans any
claimed-by-it coroutine with that bit (its resume chain covers it). Resume
seeing GCSCAN spins with la_cpu_pause (bounded: scanning is wait-free and
short; the one sanctioned micro-wait, 03 §3.7). `stack_dirty_epoch` lets a
worker skip rescanning a coroutine untouched since last round (set on
resume/yield/state ops).

### 5.7.3 Why stacks need no barrier (restated precisely)
Invariant at every instant during P_MARK: every reachable object is either
(a) marked, or (b) reachable from some root that will be rescanned this
round, or (c) reachable through a heap path whose every edge existed at the
moment the last round's scan of its root completed — and any *new* heap
edge marks its target (barrier). Induction over rounds gives termination as
in §5.1. The subtle case — thread A loads ref from heap, deletes the heap
edge, keeps ref only in registers, passes it to thread B *through the heap*
— is covered because "through the heap" is barriered; threads cannot share
registers. C code holding refs outside the Lua stack must anchor them
(standard Lua API contract; 11 §11.6 for FFI pinning rules).

### 5.7.4 Global roots (workers, once per round)
registrytv, gcroot[GCROOT_MAX] (basemt, special strings — lj_obj GCROOT
enum), mainthread, strempty fixed, ctype miscmap (11), each TG's thread_L
and cur_L, the JIT: `J->trace` vector entries with `T->root link` semantics
preserved — but traces are kept alive by their prototypes as today
(gc_traverse_proto marks pt->trace chain) plus the per-TG "executing trace"
roots: each TG publishes `tg->vmstate` = current traceno while in mcode
(already the vmstate convention) — workers mark `J->trace[vmstate]` when
vmstate>0. (Cheap, conservative: keeps a trace alive while any thread runs
it. Freeing traces additionally requires the flush handshake, 08 §8.7.)
Current bridge note: GC2 global root scanning now marks each live TG's
`thread_L` and `cur_L` thread-object roots and still preserves the per-TG
temporary-buffer root. It also marks each live TG's positive `vmstate` as an
executing trace root and traverses the recorder's in-progress `J->cur` trace,
matching the legacy `gc_traverse_curtrace()` root for not-yet-published traces.
Full trace freeing still relies on the 08 §8.7 flush handshake.

## 5.8 P_WEAK and P_SWEEP transitions

P_WEAK (leader+workers, barrier still ON, mutators running):
  Drain `gc2.weaklist`: for each weak table, for each entry: if
  key-or-value (per mode) is unmarked → store nil into the entry's val
  TValue (release; uses the normal table cell, so concurrent readers see
  nil — which is the correct Lua answer for a condemned key, since fixpoint
  proved it unreachable outside weak tables). Lua 5.1 semantics only — no
  ephemeron fixpoint (matches lj_gc.c behavior; verified: gc_traverse_tab
  marks weak-key tables' values unconditionally? No — it skips per mode and
  clearing handles it; replicate the exact gc_mayclear rules incl. the
  "string keys/values are never cleared" rule, lj_gc.c:457–471).
  Concurrent `t[k]` writes into a weak table during P_WEAK could resurrect
  an unmarked key: the table SET path, only when the table is weak AND
  phase==P_WEAK (two cold checks), marks key+value before storing. That
  closes the resurrection race with near-zero cost (weak tables are rare).
  Finalizables: walk the finalizer-registered set (FINREG objects are
  listed in a lock-free registry populated at registration: ffi.gc,
  setmetatable-with-__gc on udata — see lj_gc_separateudata logic
  lj_gc.c:142): unmarked ⇒ gc2_mark it (resurrect), push to finqueue (MPSC),
  clear FINREG (run-once, like markfinalized today). Finalizer thread runs
  entries after P_SWEEP begins (ordering: reverse registration like today’s
  mmudata list — preserve by pushing in registry order and reversing).
  Current close-time bridge: cdata finalizers now separate from the root list
  into the existing `mmudata` finalizer queue instead of running directly from
  FINREG hash order; FINREG generation scans still detect pending close-time
  cdata work.
Current bridge note: `lj_gc2_mark_to_weak()` now makes `P_WEAK` visible after
the fixpoint/paranoia bridge, and `lj_gc2_legacy_weak_begin()` aliases that
helper for current callers. `lj_gc2_weak_complete()` owns the current bounded
weak-drain loop and legacy fallback decision, while `lj_gc2_weak_to_sweep()`
publishes the staged `P_SWEEP` transition and runs the existing sweep-entry
handshake after a successful `P_WEAK -> P_SWEEP` CAS. This preserves the
original `MARK -> WEAK -> SWEEP` phase shape for
follow-up work, but legacy atomic still supplies the stop-the-world oracle; the
independent weak-table worklist and full concurrent weak/sweep phase ownership
above are not implemented yet. GC2 traversal now stores weak-table discoveries
in a bounded GC2-owned side vector
(`weak_stack`/`weak_count`) with per-slot ready publication, and counts them by
mode (`weak_tables_seen`, `weak_tables_weakkey`, `weak_tables_weakval`,
`weak_tables_allweak`, `weak_tables_queued`, `weak_tables_overflow`) without
linking through `GCtab.gclist`, because the legacy bridge still owns that link
for `g->gc.weak`. Snapshot readers expose only the contiguous ready prefix, so
reserved-but-unpublished MPSC slots are not scanned. `lj_gc2_weak_snapshot_scan()`
is a bounded, read-only oracle over that vector that mirrors the legacy weak
clear predicate, advances through the published ready prefix with
`weak_scan_cursor`, and publishes scan telemetry. `lj_gc2_weak_snapshot_clear()`
applies the same predicate with release nil stores, advances through the
published ready prefix with `weak_clear_cursor` without moving past
reserved-but-unpublished slots, and now runs before the legacy weak-clearing
fallback. `lj_gc2_weak_drain()` is the phase-gated bounded driver used by
`lj_gc2_weak_complete()` in `LJ_GC2_WEAK_DRAIN_BATCH` chunks while the full
worker-owned weak drain is staged. The current bridge uses the legacy color
bits as a stop-the-world weak-clear oracle while `g->gc.state == GCSatomic`,
so GC2 weak-key/all-weak clearing matches the legacy `gc_mayclear()` predicate
even though GC2 stack rescans are intentionally more conservative. The helper
skips legacy `gc_clearweak()` after `lj_gc2_weak_snapshot_covers_legacy()`
proves the current-cycle snapshot was fully published, fully clear-drained, and
covers every table in the final legacy `g->gc.weak` list. When the snapshot is
complete but misses legacy weak-list entries, the owner backfills those tables
with the same captured-mode clear predicate before skipping legacy clearing.
Incomplete, overflowed, or invalid snapshots still fall back to the legacy
pass.
String-bearing weak hash slots now follow legacy `gc_mayclear()` semantics in
the GC2 clear driver: strings are marked but are not themselves weak-cleared,
while a collectable key/value on the other side can still clear the entry. The
current-cycle weak mode is captured in table GC flags at GC2 discovery, so a
later `mt.__mode` mutation cannot change how the already-snapshotted table is
cleared in `P_WEAK`, and late-write barriers prefer the same captured mode
before falling back to current metatable lookup for uncaptured weak tables. The
FFI finalizer table is explicitly excluded from the weak snapshot because it is
owned by the FINREG/finalizer path, not weak-table clearing. GC2 now mirrors
cdata FINREG mutation telemetry from `ffi.gc(cd, fn)`, explicit
`ffi.gc(cd, nil)` clears, ctype `__gc` registrations, and legacy
`mmudata` queue/finalizer-driven cdata clears. FINREG queue hooks now mark
queued finalizable objects when reached during a GC2 MARK/WEAK phase, but
legacy still owns protected callback execution and drain ordering. Userdata
FINREG telemetry also mirrors C/API `__gc` metatable assignment/clear events
and counts the legacy `mmudata` queue point in `lj_gc_separateudata()`. The
original bridge left userdata finalizer membership and execution on the legacy
path; it now also mirrors the legacy userdata finalizer run-once clear at
`gc_finalize()`, and uses a userdata-only FINREG membership bit to mirror lazy
membership additions and stale clears if the metatable's `__gc` field is mutated
in place before separation. The traversal harness covers both in-place behavior
directions: a lazy add runs once, and a stale clear suppresses finalization. The
GC2 finalizer queue now links dedicated queue nodes instead of reusing queued
objects' `gcw` root/list links; callback execution and ordering remain owned by
the legacy finalizer drain bridge.
The first
weak-write bridge is present for new weak-table hash keys:
`lj_tab_newkey()` calls `lj_gc2_barrier_weak_key()` during `P_WEAK`, marking a
collectable inserted key immediately for weak-key/all-weak tables and for the
strong-key side of weak-value tables. `lj_meta_tset()` also marks collectable
weak keys when it reuses an existing hash slot, including nil-valued dead-key
slots left behind by weak clearing. C API table setters that bypass normal
legacy barriers also call `lj_gc2_barrier_weak_write()` to mark collectable
inserted keys and values; when `lua_settable()`/`lua_setfield()` resolve a
table-valued `__newindex` chain, they use the resolved owner table rather than
the original proxy for that weak-write barrier. VM `vmeta_tset` stores through
the same table-valued `__newindex` shape now add a resolved-owner weak-value
barrier after the CAS-published store, relying on the existing VM key barrier
for weak-key/all-weak key preservation. FFI cdata metatype
`__newindex = weak_table` uses the same resolved-owner bridge for
`ffi_meta___newindex`, with dedicated coverage in
`tests/t-m8-ffi-weak-newindex.c`. The traversal harness also pins the raw C API
setter path directly: `lua_rawset()` all-weak hash insertion and `lua_rawseti()`
weak-value array insertion both keep late `P_WEAK` keys/values alive through
the same bridge. During weak clearing, a current-cycle GC2 mark produced by one
of these late writes wins over stale legacy white while `GCSatomic` remains
open, so queued weak snapshots cannot clear a value that was just rescued by the
P_WEAK bridge.
`weak_keys_marked` and `weak_values_marked` expose first-time marks from these
bridges for follow-up tests. x64 VM single-value fast table stores now route
their GC2 barrier through `lj_gc2_barrier_tv_pair_g()` with the destination
table parent, and `BC_TSETM` routes the post-copy destination range through
`lj_gc2_barrier_tvn_pair_g()` with the destination table parent before the
existing table-rescan bridge. This keeps constructor batch stores from
depending on a whole-table rescan for weak-value marking after
`lj_tab_storetvn()` publishes the slots, while preserving table/backing memory
coverage for resized arrays and giving generational remembered-set filtering
parent context.
P_SWEEP entry handshake: {DISABLE_BARRIER, RESET_ALLOC, FLUSH_SSB(last)}.
  After it: workers sweep global/orphan arenas + huge table (free unmarked
  huge via munmap, deferred one epoch); owners lazy-sweep per 04 §4.6.
  String table sweep is its own protocol (06 §6.5.4) driven by a worker.
  When all arenas have sweep_epoch==cycle (workers finish stragglers of
  threads that allocate slowly): aggregate live_estimate, compute next
  trigger (§5.11), → P_IDLE.
Current bridge note: legacy sweep still owns the string/root sweep state
machine and release-publishes `lj_gc2_sweep_legacy_ready()` after legacy
string/root sweep and boundary preparation reach the final real `P_SWEEP`
boundary. `lj_gc2_sweep_to_idle()` waits for that latch plus the worker token,
rechecks phase and traversable sweep predicates, records real `SWEEP -> IDLE`
publications with `sweep_to_idle`, aggregates live estimates, and updates
pacing. Parked workers may drain traversable arena sweep before the latch, but
they publish `P_IDLE` only after the latch and all sweep predicates are clear.
The legacy C GC driver now polls safepoints between `gc_onestep()` state-machine
steps so a worker-initiated close handshake can complete during synchronous
`lua_gc()` driving. Legacy sweep still owns the final Lua GC state transition to
`GCSpause`; full scheduler ownership of that state machine remains staged.
The partial-cycle full-GC fast-forward path still calls
`lj_gc2_legacy_preserve_abort()` instead of entering `P_SWEEP`; that path now
records real active-phase aborts with `preserve_abort_to_idle` and retains the
legacy close wrapper for its preserving sweep. Sweep-to-idle closure now also
aggregates swept traversable arena live cells and marked
traversable HugeTab entries across the TG list into `gc2.live_estimate`, records
`sweep_live_updates`, and exposes the huge contribution through
`sweep_live_huge_bytes`. GC2 also owns the live-TG sweep preparation and pending
traversable sweep-list predicates used by the legacy bridge before final idle
publication. Pacing uses the larger of that GC2 estimate and the legacy
`g->gc.estimate` until raw/plain arena aggregation is independently owned by
GC2. The original bridge gated legacy boundary-lazy sweep on `mmudata != NULL`,
but the parked worker could still see `P_SWEEP` after the last item was unlinked
and while its finalizer callback was running. GC2 now publishes
`finalizer_active` around legacy `gc_finalize()`, exposes
`lj_gc2_finalizer_pending()`, and uses an owner-aware sweep predicate to block
traversable worker sweep progress and `lj_gc2_sweep_to_idle()` while finalizers
are queued or running on another TG. The current finalizer owner may still finish
a nested full-GC sweep so `collectgarbage('collect')` inside `__gc` cannot
self-deadlock. If a finalizer-spawned worker outlives the callback, the current
full-GC bridge returns to the mutator with GC still in finalization state
instead of waiting forever for `mt_live == 0`, and explicit
`collectgarbage("step", ...)` keeps `GCSfinalize` open instead of reporting a
completed cycle while that worker is still live. User finalizer callbacks now
run on the owner-claimed collector caller `lua_State` instead of the shared
`vmthread(g)` stack. Pending finalizer objects are retained through dedicated
GC2 queue nodes and marked through `lj_gc2_finalizer_mark_all()`, whose GC2
side owns the owner-drained queue scanner, but full
scheduler-owned string/root/finalizer sweep driving and FINREG/finqueue
execution remain follow-up work.

## 5.9 Deferred reclamation (grace periods) — the GC as universal SMR

Lock-free structure retirement (old table gens, old string-table vectors,
old ctype vectors, unlinked dead strings, unmapped arenas) uses:
`defer_free(p, kind)` pushes onto `deferred[hs_epoch & 1]`; the leader,
when starting a handshake that bumps hs_epoch, first frees everything in
`deferred[new_epoch & 1]` — i.e., anything retired ≥1 full epoch ago. The
soundness is I-4: readers hold raw interior pointers only between
safepoints; a full epoch means every thread polled (or was in native, where
it can't hold Lua-heap interior pointers) since retirement. Additionally
the leader injects an idle "tick" handshake every LJ_GRACE_TICK_MS (=50) so
deferral drains even with the GC idle. (For table/strtab gens that are also
GC-traceable via their owner, defer_free is belt-and-braces over normal
sweep — both paths are kept because gens are raw, not GC objects: DECIDED
gens are raw allocations carved from non-traversable arenas with their own
block bits and freed *only* via defer_free; sweep treats cells with the
AF_RAWGEN arena flag as opaque. Simplest ownership story.)

## 5.10 collectgarbage() mapping
"collect": requester triggers cycle, then *parks* (allowed block) until
phase returns to P_IDLE with cycle>seen. "step": hint the leader; returns
false. "stop"/"restart": gate the trigger. "count": 04 §4.8.
"setpause"/"setstepmul": map to gcpause_pct / assist_shift. New:
"torture" (§5.13), "workers", N.

Current bridge: while secondary Lua threads are live, explicit `collect` and
`step` do not run the legacy collector. They request a GC2 cycle through the
same nonblocking leader token used by allocation triggers, store the pending
threshold in the MT threshold mirror, and perform only bounded worker-drain
assistance before returning. Active-thread explicit `step` bypasses the
automatic-trigger stop gate, matching the legacy single-thread behavior where
`collectgarbage("step")` restarts GC after `collectgarbage("stop")`. Active
stopped `collect` requests a one-shot major cycle and restores the stopped
threshold when that requested full cycle later reaches idle. Exact `collect`
parking still waits on the GC2 leader path that can close sweep without legacy
driver ownership.

## 5.11 Pacing & assists
Trigger when `alloc_since_trigger > trigger_bytes` where trigger_bytes =
live_estimate * gcpause_pct/100 (default 100% ⇒ 2x heap growth, Lua-like).
Hard limit `hard_bytes = trigger*2`: an allocating thread past it runs a
bounded **mark assist** in alloc_slow: pop ≤2^assist_shift objects from the
SSB-stack/steal and trace them (mutator tracing reuses worker code with
tg-local scratch). Current bridge extension: once a cycle is in `P_WEAK`,
the same hard-limit assist token spends any remaining bounded work budget on
`lj_gc2_weak_drain()`, advancing the weak clear cursor without adding locks and
attributing that work through `assist_weak_drained`.
This bounds heap growth under worker starvation without ever blocking. Leader
spawns workers `min(ncpu-1, max(1, live/64MB))`,
parked on futex between cycles.

## 5.12 Generational mode
Inherited from the bitmap design at near-zero cost: minor cycle = same
mark machinery but roots = stacks + SSB-remembered set only, and sweep uses
the minor identity (mark' = block|mark keeps survivors black). Remembered
set: the store barrier *stays enabled between cycles in gen-mode* but
degenerates to "if target arena is OLD and stored obj arena is YOUNG →
SSB" — i.e., classic card-less remembered set via the same SSB. Heuristic
switch exactly as Pall describes (survival-rate driven). This is M10
(post-perf-gate) — land the flag and the sweep identity early, enable late.
Current bridge: `collectgarbage("generational")` /
`collectgarbage("incremental")` toggles `GC2State.generational` and exposes the
mode through `collectgarbage("stats")`. Full GC publishes a one-shot major
override. Entering generational mode also publishes a one-shot major baseline
before later generational allocation-triggered mark begins record minor-cycle
requests. Idle generational barriers conservatively queue remembered entries
into SSB without draining outside a cycle and force a major on overflow; the
actual minor execution path stays behind a first-major-baseline gate. Minor
sweep identity is routed through a latched `cycle_sweep_minor` flag and public
`minor_sweep_enabled` gate, which turns on with `minor_roots_enabled` after that
baseline completes. Parent-aware table/object, fast
table-value, resolved meta-store, and closed-upvalue value barriers now use that
gate to filter for old-parent/young-child remembered pairs. The remaining
parentless TValue barrier users are root-slot wrappers, and the stale x64
value-only VM helper has been removed. Completed minor cycles sample flushed
allocation bytes at mark begin, estimate young survival from
`live_after - previous_live`, and force a one-shot major when the default
survival threshold is reached.
Minor root selection has matching `cycle_roots_minor` /
`minor_roots_enabled` latches and `minor_roots_deferred` telemetry. The
`HS_SCAN_ROOTS` bridge routes through a cycle-root selector, so public cycles
after the baseline can use the minor root scanner.

## 5.13 Torture & debug
`collectgarbage("torture",1)`: leader runs continuous back-to-back cycles
with handshake every allocation-slowpath; plus `LJ_GC2_PARANOIA` build:
after fixpoint, stop-the-world ONCE (debug builds only) and run the legacy
full mark to diff reachable-vs-marked sets — the single most valuable
correctness oracle; implement it at M3 before anything else relies on the
GC.

## 5.14 File/function manifest (what M3 actually writes)
lj_safepoint.{h,c}: tg list, handshake, ack, native enter/leave, poll C
fallback `lj_safepoint_poll(L)`.
lj_gc2.{h,c}: GC2State, mark/traverse (ported per §5.6.4), deques, SSB,
weak/finalize, sweep driver, pacing, leader thread loop, torture, paranoia.
lj_gc2_barrier.h: the C store-barrier inline used by lj_tab/lj_func/api:
```c
static LJ_AINLINE void lj_gc2_wbarrier(TGState *tg, cTValue *v) {
  if (LJ_UNLIKELY(tg->mark_active) && tvisgcv(v)) gc2_mark(tg, gcV(v));
}
/* and the obj/GCRef variants; every legacy lj_gc_barrier* call site maps
   to one of these — grep list in 12 §M3 (≈35 sites in lj_api.c, lj_tab.c,
   lj_meta.c, lj_func.c, lj_state.c, lj_cdata.c, lj_ccallback.c, lib_*) */
```
Asm barrier: 07 §7.4. JIT barrier: 08 §8.8.

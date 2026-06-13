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

## 5.3 State (struct GC2State, in global_State)

```c
typedef struct GC2State {
  uint32_t phase;              /* P_*; written by leader               */
  uint32_t cycle;              /* monotonically increasing cycle id    */
  uint64_t hs_epoch;           /* handshake generation (see §5.4)      */
  uint32_t hs_pending;         /* acks outstanding (atomic countdown)  */
  uint32_t hs_actions;         /* HS_* bits of current handshake       */
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
remain the original target above.

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
so current cycles exercise the detector. The original full loop above remains
the target: a scheduler-owned pool, per-worker idle declaration, and leader
ownership of the `P_WEAK` transition are still follow-up work.

### 5.7.2 Thread stacks
`HS_SCAN_ROOTS` (mutator, at ack): for its *running* L (and the chain of
parents if inside coroutine.resume — walk `L->cframe`/frame links exactly
like gc_traverse_frames lj_gc.c:292): mark every TValue in
`stack..top`+frame extras, mark L itself, its env, legacy openupval list
values. This is precise and fast (linear TValue scan; ~1 GB/s ⇒ a 1 MB
stack costs ~1 ms, typical stacks ≪ that). Current bridge implementation
loads each stack slot into a local snapshot before marking; the original
ownership/claim protocol below remains the target for suspended coroutines.
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
Current bridge note: `lj_gc2_legacy_weak_begin()` now makes `P_WEAK` visible
after the fixpoint/paranoia bridge and before legacy `gc_clearweak()`. This
preserves the original `MARK -> WEAK -> SWEEP` phase shape for follow-up work,
but legacy weak clearing remains authoritative; the weak-table worklist and
full concurrent weak clearing above are not implemented yet. The first
weak-write bridge is present for new weak keys: `lj_tab_newkey()` calls
`lj_gc2_barrier_weak_key()` during `P_WEAK`, marking a collectable inserted key
immediately. C API table setters that bypass normal legacy barriers also call
`lj_gc2_barrier_weak_write()` to mark collectable inserted keys and values.
`weak_keys_marked` and `weak_values_marked` expose first-time marks from these
bridges for follow-up tests. x64 VM single-value array table stores now route
their GC2 barrier to the stored TValue directly, so weak-value arrays mark the
new value even though weak-value table traversal skips values.
P_SWEEP entry handshake: {DISABLE_BARRIER, RESET_ALLOC, FLUSH_SSB(last)}.
  After it: workers sweep global/orphan arenas + huge table (free unmarked
  huge via munmap, deferred one epoch); owners lazy-sweep per 04 §4.6.
  String table sweep is its own protocol (06 §6.5.4) driven by a worker.
  When all arenas have sweep_epoch==cycle (workers finish stragglers of
  threads that allocate slowly): aggregate live_estimate, compute next
  trigger (§5.11), → P_IDLE.

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

## 5.11 Pacing & assists
Trigger when `alloc_since_trigger > trigger_bytes` where trigger_bytes =
live_estimate * gcpause_pct/100 (default 100% ⇒ 2x heap growth, Lua-like).
Hard limit `hard_bytes = trigger*2`: an allocating thread past it runs a
bounded **mark assist** in alloc_slow: pop ≤2^assist_shift objects from the
SSB-stack/steal and trace them (mutator tracing reuses worker code with
tg-local scratch). This bounds heap growth under worker starvation without
ever blocking. Leader spawns workers `min(ncpu-1, max(1, live/64MB))`,
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

# 09. The threading.* Library

User-facing API + implementation spec. New files: `lib_threading.c`,
`lj_thr.h/.c` (OS shim: pthread create/TLS/futex), `lj_chan.h/.c`.
Registered from lib_init.c like other built-ins; `require"threading"` also
works (preload entry). Everything here is implementable after M4 of 12.

## 9.1 API surface (complete)

```
threading.spawn(f, ...) -> thread     -- start OS thread running f(...)
thread:join() -> true, results...     -- or false, errobj; idempotent
        | thread:join(timeout_s) -> nil,"timeout" if still running
thread:id() -> integer                -- stable small id (tid)
thread:running() -> boolean
threading.current() -> thread         -- the caller's thread object
threading.sleep(seconds)              -- blocking, native-state, poll-aware
threading.cpucount() -> integer
threading.fence()                     -- seq_cst full fence (02 M-3)
threading.channel(capacity=0) -> ch   -- capacity 0 = rendezvous
ch:send(v [, timeout_s]) -> true | nil,"timeout" | error "closed"
ch:recv([timeout_s]) -> v, true | nil, false (closed&empty)
                       | nil,"timeout"
ch:close()                            -- wakes all; send after close errors
ch:peek() -> v, true | nil, false     -- non-blocking
threading.mutex() -> m; m:lock(); m:unlock(); m:trylock()->bool
                                      -- convenience only; see §9.7
```
Exactly one value per send (tables for tuples). Any Lua value may be sent
or shared — the heap is shared (ADR-2); a channel transfers a *reference*
plus a happens-before edge, never a copy.

## 9.2 thread objects

A `thread` is a userdata wrapping `{ lua_State *L; TGState *tg; pthread_t;
uint32 tid; uint32 state (SPAWNED/RUNNING/DONE/JOINED); TValue results
anchor (a table); GCRef errobj; uint32 join_futex; }`. The lua_State is a
normal GC thread object (gct LJ_TTHREAD) anchored by the userdata. The
original report used a `g->gcroot`-adjacent live-threads table; the current
implementation uses a native lockless `global_State.threading_live` root list
until join/shutdown drops the entry, so an unreferenced running thread is never
collected (detach semantics: dropping the handle is legal; the thread runs to
completion; its results are dropped).

## 9.3 spawn

```
threading.spawn(f, a1..an):
  check f is a function (Lua or C). Build child L: lua_newthread under the
  caller (so it's rooted), claim it (thr_owner=parent, 06 §6.7), xmove
  f+args onto its stack, release claim with owner=0.
  pthread_create → child entry:
    lj_thr_entry: lj_tg_attach() (03 §3.2: build TG, copy dispatch
    template, register in gc2.tg_list incl. handshake catch-up: adopt the
    CURRENT phase flags — read g->gc2.phase under the attach epoch
    protocol: attach performs a self-handshake: set own mark_active/
    alloc_black mirrors from phase, ack current hs_epoch before first
    bytecode — leader counts threads via list+epoch so a mid-handshake
    attach either acks or is not counted; the attach CAS into tg_list and
    the leader's hs_pending snapshot are ordered by re-checking list
    after snapshot: leader algorithm in 05 §5.4.2 gains a "second pass
    over tg_list for entries with hs_epoch_ack < epoch" loop).
    CAS thr_owner 0→tid; lua_resume-equivalent protected call of f.
    On return: store results into the results table (barriered), state→
    DONE (release), futex_wake(join_futex), lj_tg_detach (flush SSB,
    local_total, owned arenas → global needsweep stacks, mark TG dead).
  happens-before: everything in the parent before spawn ≺ child first
  instruction — provided by pthread_create + the release/acquire on the
  thr_owner CAS pair.
```

## 9.4 join

```
thread:join(timeout):
  loop: s = la_load32_acq(&state)
    DONE: CAS state DONE→JOINED (winner pulls results; others see JOINED:
          return the cached results too — results table is immutable after
          DONE; idempotent join DECIDED)
          remove live-root node only after caller stack growth and result
          copy, so a C API joiner holding only lua_State* does not drop the
          child root before an allocating stack check
    else: lj_native_enter(tg); la_futex_wait(&join_futex, s, ns);
          lj_native_leave → poll. timeout → nil,"timeout".
  happens-before: child's everything ≺ join return (acquire on state).
  Joining yourself / join from two threads: both legal (futex broadcast).
  Main thread exiting with unjoined threads: §9.6.
```

Current bridge note: the wait path already checks STOPREQ after each native
futex wait. The final winner-side `pthread_join()` native leave now records its
action mask, completes result copying and live-root/TG cleanup, then checks
STOPREQ before returning normal join results. The original target above still
holds the live root through stack growth and result copy; shutdown delivery is
deferred until that cleanup is complete so a caught interrupt cannot strand the
joined child in the live list.

## 9.5 channels

Implementation: bounded MPMC ring of TValue slots + seq counters
(Vyukov-style bounded queue — per-slot sequence numbers, fetch_add on
enqueue/dequeue tickets; lock-free for senders/receivers; the canonical
algorithm, ~80 lines) with futex parking lot for empty/full waits:
```
struct Chan { uint32 cap; _Alignas(64) uint64 enq; _Alignas(64) uint64 deq;
  uint32 closed; uint32 waiters_futex; ChanSlot slots[]; }
ChanSlot { _Alignas(16) uint64 seq; TValue v; }
send: ticket = fetch_add(enq); slot = &slots[ticket % cap];
      wait seq==ticket*2 (spin K=64 with pause, then native_enter+futex);
      wbarrier(v); tv_rawstore(&slot->v, v);
      la_store64_rel(&slot->seq, ticket*2+1); futex_wake.
recv: ticket = fetch_add(deq); wait seq==ticket*2+1 (spin→park);
      v = tv_rawload_acq(&slot->v) [acquire pairs with the rel above:
      the M-3 synchronizes-with edge]; clear slot (store nil — keeps GC
      from retaining sent values: slot TValues ARE GC roots, scanned via
      the channel userdata's traverse hook registered with gc2, 05
      §5.6.4 gains a Chan case); la_store64_rel(&slot->seq, ticket*2+2).
cap==0 (rendezvous): implemented as cap=1 + send waits for seq to pass
      consume state before returning (sender returns only after a receiver
      took it) — flag-checked in send tail.
close: la_store32_rel(closed,1); futex_wake all. Parked tickets re-check:
      receivers past `enq` snapshot return nil,false; senders error.
      Ticket reservations vs close: a parked sender whose ticket was
      already issued completes the send (documented: close is graceful).
timeout: futex timed wait; on timeout the ticket must be *cancelled*:
      DECIDED simplification — timeouts only at ticket-acquisition stage:
      implementation takes tickets optimistically ONLY when a matching
      counterpart is provably available (compare enq/deq), else parks on
      the counters first (no ticket held) — this is the two-phase variant;
      port it from the model the agent writes first as a C unit test
      (13 §13.6.2 chan_stress.c) before wiring to Lua.
```
Current implementation note: `lj_chan.c` uses CAS loops to claim enqueue and
dequeue tickets, but keeps the same per-slot sequence protocol. Channel slot
payloads now use `chan_storetv_rel()`, `chan_loadtv_acq()`, and
`chan_cleartv_rel()` so the shared `TValue` word is never copied with plain C
struct assignment; the sequence counter release/acquire still provides the
channel happens-before edge. Lua-facing receives use `lj_chan_recv_timeout_gc()`
to apply a root barrier before clearing the channel slot, so the previous
channel root is not lost before the result is copied onto the Lua stack. Legacy
GC and GC2 traversal also acquire-snapshot channel slots into local `TValue`s
before marking.

Thread userdata links are published through `lua_State.mt_thread` with
`setgcrefrel()` in `threading_state_set_ud()` and read back with
`gcref_acq()` in join/attach lookup plus legacy GC/GC2 thread traversal. This
keeps the child-state backlink visible without adding an `LJ_MT` lock gate. The
private-environment GC root is release-published through `setgcrefroot()` and
acquire-loaded in lib_threading readers. The old live-table root from the
original report has been replaced by CAS-published `LJThreadLive` nodes whose
`GCRef ud` is release-published and acquire-scanned by legacy GC and GC2.
Current implementation note: spawned child stacks are now moved to the child
TG's arena before the pthread is published/started. The original spawn flow
created `lua_newthread()` stacks in the parent/main arena, which was faithful
to legacy single-thread allocation but not to the per-owner arena rule from
04. `lj_state_rehome_stack()` preserves the original stack contents and pointer
offsets, accounts the allocation to the worker TG, and frees the parent-arena
stack while only the spawning thread can touch it.

The temporary M4 active-child GC pause treats `g->gc.threshold` and the saved
`g->mt_gc_threshold` as shared handoff words. C-side checks and updates go
through `lj_gc_threshold_*()` / `lj_gc_mt_threshold_*()` helpers, and
`lua_gc(stop/restart)` replays its saved logical threshold to the real
threshold if `mt_live` reaches zero during the update. Explicit legacy
`collect` / `step` claim `mt_gc_exclusive` only after observing `mt_live == 0`
and rechecking it after the claim; secondary `spawn` / `luaMT_attach()` entry
waits out that slow-path word before incrementing `mt_live`. If peers are
already live, explicit `collect` / `step` now request a GC2 cycle and save the
pending driver threshold in `mt_gc_threshold` instead of running the legacy
collector concurrently. The active-thread `step` request bypasses the
automatic-GC stop gate, matching the legacy single-thread restart behavior.

The channel userdata is shared freely; all ops are method calls
(lib_threading.c → lj_chan.c). GC: channels live in non-traversable? NO —
traversable arenas; traverse = mark in-flight slot values (bounded scan of
cap slots, reading seq parity to know occupancy — racy reads fine: marking
a just-consumed value is conservative-safe; missing a just-produced one is
prevented because the producer's wbarrier already marked it during P_MARK).

## 9.6 process & main-thread semantics

- The main thread returning from the top-level chunk (or os.exit) triggers
  VM shutdown: leader issues `HS_STOPREQ`; every other mutator, at next
  poll/park-wakeup, raises error "thread interrupted: VM shutdown" through
  its pcall chain (unprotected → thread just ends). Main then joins all,
  runs pending finalizers (bounded by 2 cycles), lua_close.
  Current x64 VM safepoint polls call `lj_safepoint_ack_check()`, which
  applies pending actions and immediately raises on `HS_STOPREQ`. The x64
  branch trigger currently checks `g->gc2.hs_pending` so secondary Lua threads
  see pending handshakes while the VM dispatch base remains tied to the
  embedded main `GG_State` layout. Native waits keep using
  `lj_safepoint_checkstop()` after wake/leave.
- lua_close from C: same, plus asserts caller is the main thread.
  Shutdown stores `mt_shutdown`, rejects racing spawn/attach entrants, and
  waits for `mt_live` to reach zero with a futex wait/wake so external
  `luaMT_attach()` users are gone before state teardown even if they have no
  joinable `threading.thread` handle. A TG that detaches with a pending
  handshake request self-acks before clearing its signal words, preserving
  `gc2.hs_pending` accounting.
- A crashed thread (error escaped f): error object stored, state DONE;
  nothing else dies (Go-style panics-are-isolated DECIDED; an
  `threading.onerror(fn)` hook is v2).
- `threading.current()` returns the cached `__main` handle only for
  `mainthread(g)`. External `luaMT_attach()` states that were not created by
  `threading.spawn()` have no joinable thread userdata and error from
  `current()` until a dedicated non-joinable handle model is added.
- Deadlock (all threads parked, no GC work): not detected in v1; document.

## 9.7 mutex (and why it exists)

The *runtime* never needs it; user code coordinating external resources
does. m = futex word; lock: CAS 0→1 else CAS→2+futex_wait (classic
3-state); unlock: xchg 0 + wake if was 2. lock() enters native state
(counts as blocking API per 02 §2.2). Holding a mutex across a GC
handshake is fine (mutators never wait on each other's acks).

## 9.8 math.random
Per-TG PRNG (03 §3.3): streams are independent per thread;
`math.randomseed(s)` seeds the calling thread; spawn derives child seed =
splitmix(parent_seed, tid) so unseeded programs don't correlate. Document.

## 9.9 C API additions (lua.h additions)
```
lua_State *luaMT_spawn(lua_State *L, int nargs);   /* like spawn, f+args on stack */
int  luaMT_join(lua_State *L, lua_State *child, lua_Number timeout);
void luaMT_fence(void);
int  luaMT_attach(lua_State *L);  /* adopt foreign OS thread: builds TG,
        claims a fresh lua_State; for embedders driving their own pools */
void luaMT_detach(lua_State *L);
```
lua_lock/lua_unlock remain no-ops (the design point of the whole project).
Rules for embedders: a lua_State may be used only by its owner thread or
after explicit claim (06 §6.7); document loudly in luajit.h comments.

## 9.10 allocator note
Per 04 §4.9: lua_newstate's allocf only feeds arena/huge mappings under
LUAJIT_MEM_HOOK; otherwise ignored (one-time vmevent warning).

## 9.11 Examples (ship in doc/ and as tests t-api-*.lua)
```lua
local th = require"threading"
-- 1: parallel map
local function pmap(xs, f, n)
  local work, done = th.channel(#xs), th.channel(#xs)
  for i,x in ipairs(xs) do work:send({i,x}) end; work:close()
  local ws = {}
  for i=1,(n or th.cpucount()) do
    ws[i] = th.spawn(function()
      for job in work.recv, work do done:send({job[1], f(job[2])}) end
    end)
  end
  local out = {}
  for _=1,#xs do local r = done:recv(); out[r[1]] = r[2] end
  for _,w in ipairs(ws) do assert(w:join()) end
  return out
end
-- 2: shared upvalue across threads (the requirement-3 smoke test)
local counter = 0
local c = th.channel(0)
local t = th.spawn(function() counter = counter + 1; c:send(true) end)
c:recv(); assert(counter == 1)  -- cell write happens-before recv return
t:join()
```

## 9.12 Memory-model restatement for the manual page
Channels/join/spawn/fence are the only synchronization primitives (02 M-3).
Plain shared-table traffic is racy-but-safe (M-1/M-4/M-5). Copy the table
from 02 §2.1 into the user manual verbatim.

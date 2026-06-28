# 01. Architecture Overview & Decision Records

This document fixes the architecture. Every "ADR" (Architecture Decision
Record) below is binding (see 00 §0.4 rule 6). Detailed designs live in the
referenced documents; this file is the map.

## 1.1 The shape of the system

```
                    one universe (one global_State `g`)
 ┌──────────────────────────────────────────────────────────────────────┐
 │  SHARED, LOCK-FREE                                                   │
 │   • heap: arenas + huge blocks (04)        • string intern table (06)│
 │   • all GC objects incl. all lua_States    • trace cache + mcode (08)│
 │   • registry, gcroot[], basemt[]           • CTState (11)            │
 │   • GC control state `g->gc2` (05)         • dispatch *templates*    │
 ├──────────────────────────────────────────────────────────────────────┤
 │ PER OS THREAD: the TG block (DISPATCH points here)            (03)   │
 │   • dispatch table copy + hotcount[64]     • safepoint word + reqs   │
 │   • allocator (owned arenas, bump ptrs)    • grey SSB (mark buffer)  │
 │   • cur_L, jit_base, tmpbuf, prng, …       • native-state flag       │
 ├──────────────────────────────────────────────────────────────────────┤
 │ THREADS                                                              │
 │   mutator threads (threading.spawn)  ── each runs one lua_State,     │
 │       may own many suspended coroutines                              │
 │   GC worker threads (N≈cores/4, ≥1)  ── mark + sweep + huge-free     │
 │   finalizer thread (1, lazy-started) ── runs __gc / ffi.gc           │
 └──────────────────────────────────────────────────────────────────────┘
```

Execution model: `threading.spawn(f, ...)` creates an OS thread bound to a
fresh `lua_State`. Coroutines keep today's semantics *within* an OS thread.
Any Lua value — tables, closures, coroutines, cdata, strings — may flow
between threads by ordinary assignment because the heap is shared. The
runtime guarantees **memory safety and type safety under arbitrary data
races**; it guarantees **sequential consistency only through `threading.*`
synchronization operations** (02 §2.4). That is the same contract Java/Go
give their users, and it is the only contract compatible with "lockless".

## 1.2 Decision records

### ADR-1 — Platform: x86-64 Linux, LJ_GC64 mandatory. DECIDED.
Why: with `LJ_GC64` a `TValue` is a single NaN-tagged 64-bit word
(src/lj_obj.h:174–213) and a `GCRef` is a full 64-bit pointer
(lj_obj.h:53–60). Aligned 8-byte atomic loads/stores are free on x86-64,
so a Lua value is the natural atomic unit and tearing is impossible. On
32-bit targets a TValue is two words and every slot access would need wide
atomics or seqlocks — an unacceptable tax. x64 already defaults to GC64
(lj_arch.h `LJ_TARGET_GC64`). Enforce:
```c
#if !LJ_GC64 || !LJ_TARGET_X64 || !LJ_TARGET_LINUX || LJ_TARGET_WINDOWS
#error "lockless runtime requires GC64 on x86-64 Linux"
#endif
```

### ADR-2 — One shared global_State; threads are lua_States. DECIDED.
Requirement 3 (cross-thread upvalue mutation) forces a shared heap; isolated
universes with message passing (Lanes/llthreads style) cannot express it.
`lua_State` already is the per-execution-context object (stack, frame, open
upvalue list, status — lj_obj.h:691–705); we reuse it unchanged as the
per-OS-thread context and add a thread-ownership word (06 §6.7).

### ADR-3 — GC: arena heap (Pall LJ3 design) + on-the-fly parallel
grey-stack Dijkstra marking with soft-handshake fixpoint (FUGC family).
DECIDED. The arena layout (64KB-aligned arenas, 16-byte cells, segregated
block/mark bitmaps with differential encoding, bitmap-only sweep, huge-block
side table) is taken from Pall's LuaJIT 3.0 GC design document and kept
almost verbatim because it is precisely what a concurrent collector wants:
mark state outside objects (atomic bitmap ops, no object dirtying), sweep
that never touches object memory (trivially parallel + lock-free per-arena),
and allocation that is per-arena and therefore per-thread once arenas have
owners. The *marking protocol* on top is replaced: instead of Pall's
incremental mutator-driven stepping we run dedicated GC workers concurrently
with all mutators, coordinated only by soft handshakes (no stop-the-world),
using a Dijkstra **insertion** store barrier active only during the mark
phase, black allocation during marking, repeated root re-scan rounds until a
fixpoint. This is the FUGC/DLG lineage; FUGC is the production existence
proof that the exact combination (non-moving heap, phase-gated CAS-relaxed
store barrier, grey stacks, handshake fixpoint, parallel bitmap sweep) works
and is fast. Full design: 05. SATB/Yuasa deletion barrier was evaluated and
rejected for v1 (old-value load on every overwrite store; needs
snapshot-ordered stack scans); it is preserved as fallback F-2 in 14 §14.3.

### ADR-4 — Allocator: per-thread arena ownership, bump-first, size-class
fit fallback; huge blocks in a lock-free side structure. DECIDED.
Replaces `lj_alloc.c` (a process-global dlmalloc derivative — every
allocation today also links the object into `g->gc.root`, a single global
list: lj_gc.h:122–135, lj_obj.h GCHeader `nextgc`). The new allocator never
shares a fast path between threads: each thread owns its open arenas and
sweeps them itself ("owner-sweeps"), so the fast path has zero atomics.
Global coordination only on arena acquisition/retirement (lock-free Treiber
stacks with ABA counters). The `nextgc` linked list is **deleted**; the
header byte pair `marked`/`gct` is repurposed (04 §4.7). Full design: 04.

### ADR-5 — Safepoints: cooperative polls + soft handshakes; no signals,
no thread suspension. DECIDED. Poll sites: interpreter backward jumps and
function entry (07 §7.3), JIT loop backedges and exit-to-interpreter paths
(08 §8.6), allocation slow path, and every `threading.*` blocking entry.
Threads in C/FFI calls are in **native state** and are handshaken *for*
them by the GC (05 §5.4.3, 11 §11.5). Cost when idle: one TLS-relative load
+ predicted-not-taken branch per poll site.

### ADR-6 — Tables: per-generation node/array vectors; lock-free reads
always; CAS writes; cooperative resize with FORWARD sentinels; reclamation
via GC + handshake grace. DECIDED. Brent-variation node relocation in
`lj_tab_newkey` (lj_tab.c:436+) is removed — nodes never move within a
generation, which is also what keeps HREFK valid. Full design: 06 §6.2–6.3,
executable model: aux/nbtab_model.c.

### ADR-7 — Upvalues: cell model (always-closed upvalues), new bytecodes
CNEW/CGET/CSET, no open upvalues in v4 chunks. DECIDED. Open upvalues alias
live stack slots of one thread; sharing them across threads would force
cross-thread stack access and pin stack reallocation. Cells keep exact Lua
sharing semantics (closing preserves GCupval identity) while making every
upvalue a stable heap word. Legacy (v2) chunks keep the old machinery with a
documented capture-at-FNEW deviation under MT. Full design: 06 §6.4, 10.

### ADR-8 — JIT: single-recorder token; immutable published traces;
side-exit indirection table instead of code patching; dual-mapped mcode for
W^X; bytecode patches as single atomic 32-bit stores. DECIDED. Recording is
rare; serializing *recording* (not execution) through one CAS-acquired token
preserves "lockless hot paths" while avoiding a jit_State replication
project. Published machine code is never modified; all retargeting goes
through data (exit tables, link words, bytecode words). Full design: 08.

### ADR-9 — threading.* API: structured spawn/join + channels + shared
memory; runtime-internal blocking only via futex park. DECIDED. Spec: 09.

### ADR-10 — Hooks, profiler, jit.* control: universe-global in v1,
serialized through the recorder token; per-thread hooks deferred. DECIDED
(documented limitation; see 03 §3.6, 14 §14.2-L3).

## 1.3 What "as close to original performance as possible" means here

Costs we accept and their bounds (measured gates in 13):
- safepoint poll: ~2 instructions on backward jumps/calls; <1% interpreter,
  <0.5% JIT (hoisted to loop backedge which already pays hotcount).
- store barrier: 2 instructions when GC idle (load TG flag + jnz), active
  only during mark phase; array stores gain the same check (today they have
  none).
- table hash ops: same probe sequence as today minus Brent relocation; one
  extra acquire load of the gen header per op start.
- allocation: bump path is *shorter* than today (no global list link, no
  `g->gc.total` RMW — accounting is thread-local, 04 §4.8).
Wins we bank: parallel GC off the mutator entirely (today `lj_gc_step` runs
*on* the mutator: lj_gc.c:724), bitmap sweep, no allocator lock, generational
mode retained (05 §5.12).

## 1.4 Invariant index (the ones everything hangs on)

I-1  A TValue/GCRef in shared memory is only accessed through lj_atomic ops;
     8-byte aligned; never torn. (02)
I-2  During P_MARK every store of a collectable ref into the heap marks the
     stored object (Dijkstra). Stack slots are exempt; stacks are re-scanned
     each handshake round until fixpoint. (05 §5.5–5.7)
I-3  Objects allocated during P_MARK are born black. (05 §5.5)
I-4  No safepoint poll inside a lock-free critical sequence; therefore any
     pointer loaded inside one remains valid until the next poll, and
     retiring a structure after one full handshake round is safe. (05 §5.9)
I-5  Within a table generation, nodes never move and keys are written once
     (nil-sentinel → key, via CAS). (06 §6.2)
I-6  Published mcode and published GCtrace bodies are immutable. (08)
I-7  A lua_State is executed by at most one OS thread at a time, enforced by
     a CAS-claimed owner word. (06 §6.7)
I-8  String identity: at most one live interned GCstr per content. (06 §6.5)

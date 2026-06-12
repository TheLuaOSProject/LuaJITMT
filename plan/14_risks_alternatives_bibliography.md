# 14. Risks, Fallbacks, Alternatives, Bibliography

## 14.1 Risk register (top items, with detection + response)

R1 **Single-thread regression >10%** (the headline risk). Detect: M2/M3/M6
gates. Largest suspected contributors in order: store barrier on array
writes; table gen-header indirection; cell ops vs raw slot upvalues;
safepoint polls in call-heavy code. Responses: M9 menu; then fallback F-1
(elide array-store barrier by making array parts grey-retained: tables
with arrays re-scanned at fixpoint rounds like stacks — trades mark work
for mutator speed; sound because rescan-to-fixpoint covers exactly the
unbarriered-container case); F-3 (poll only on hotcount underflow).

R2 **Fixpoint non-termination under adversarial mutation** (rounds keep
finding work). Theoretically bounded (05 §5.1: marked set monotone ≤ live
set) but *wall-clock* can stretch if mutators allocate grey work faster
than workers drain. Detect: cycle-time watchdog + marks_this_round trend.
Response: assists already bound it (05 §5.11); escalate assist_shift; the
hard backstop is raising trigger so cycles start earlier.

R3 **Lock-free table bug class** (lost update / duplicate key / UAF).
Mitigation is structural: the model file is the algorithm, ported; TSAN
unit drivers + fuzz_tabops + 10-min soaks per commit. Worst-case fallback
F-4: per-table seqlock on the *resize path only* (readers stay lock-free;
writers serialize during migration) — contained, still meets §2.2 for hot
paths.

R4 **ABA/grace bugs in defer_free** (reader holds pointer across an
epoch). The invariant is I-4; enforcement is code review + the
LJ_NOSAFEPOINT audit comments + defer_free_test.c under ASAN. Response if
violated in the wild: lengthen grace to 3 epochs (constant) while fixing
the real leak of a poll into a critical sequence.

R5 **membarrier/memfd availability** (old kernels, containers). Detect at
init; fallbacks: memfd→RWX flag (08 §8.5); membarrier→`mprotect`-based
IPI trick or global `sys_membarrier(GLOBAL)`; document minimum kernel
4.14 (memfd 3.17, PRIVATE_EXPEDITED_SYNC_CORE 4.16) — else interpreter-
only MT.

R6 **Legacy-uv deviation breaks a real workload** (10 §10.4). Response:
the `strict` knob errors loudly; ultimate fix is recompiling sources (v4),
which is always available since sources compile to cells.

R7 **Recorder token starvation** (one thread hogs recording). Penalties
naturally back off losers; add token fairness only if observed (ticket
order) — not v1.

R8 **Finalizer thread deadlocks user code** (finalizer blocks on a channel
no one serves). Same hazard class as today's __gc; document; watchdog
vmevent after 10s.

## 14.2 Accepted limitations (documented, not bugs)
L1 `next/pairs` during concurrent resize = "modified during traversal"
   semantics (06 §6.3.6). L2 collectgarbage("count") is an estimate.
L3 hooks/profiler are universe-global (03 §3.6). L4 legacy-uv capture-at-
   FNEW after thread activation (10 §10.4). L5 package.loaded double-load window (06
   §6.8). L6 classic lua_CFunction modules are unsafe unless they follow
   §6.7/§11.6 rules (requirement 4 grants this). L7 no per-op timeout on
   cdef token. L8 nilnode freetop trick removed ⇒ tiny hash tables
   allocate their NHdr eagerly on first insert.

## 14.3 Designed fallbacks (pre-approved deviations)
F-1 grey-retained arrays (R1). F-2 **SATB/Yuasa deletion barrier** as the
whole-protocol alternative: barrier captures *overwritten* values during
marking; stacks scanned once at start (snapshot); no fixpoint rounds.
Pros: single root-scan; cons: old-value load on every overwriting store,
floating garbage, snapshot subtleties with lock-free tables (the FORWARD
freeze must log displaced values). Switch only if R2 proves unmanageable;
the barrier call sites are shared, so the swap is localized to
lj_gc2_wbarrier + scan scheduling. F-3 poll placement reduction (R1).
F-4 resize seqlock (R3). F-5 single-threaded-JIT mode (token never
released to others) if M6 overruns: MT interpreter + ST JIT is still a
shippable intermediate.

## 14.4 Alternatives considered and rejected (so you don't re-explore)
A. Isolated states + message passing (Lanes model): fails requirement 3.
B. Global VM lock with GC-time scheduling (Python GIL): fails "parallel".
C. Per-object locks / lock striping on tables: fails lockless + perf.
D. Fully concurrent copying/compacting GC: violates Lua/C API pointer
   stability; Pall's own constraint list demands non-moving.
E. Hazard pointers instead of epoch grace: per-read protection cost on
   every table/string access; the safepoint system already provides
   quiescence for free.
F. Go-style hybrid write barrier (Yuasa+Dijkstra mix, no stack rescan):
   attractive (Go proposal 17503) but assumes precise stack maps +
   re-scan-free termination machinery LuaJIT lacks; rescan rounds are
   cheap here because Lua stacks are small and scanning is a linear
   TValue sweep.
G. Software transactional tables: ergonomic but not "near-original
   performance".
H. Per-thread heaps with shared-immutable + proxy objects (Erlang-ish):
   fails shared-mutable-table requirement.

## 14.5 Bibliography (design-element → source)
- Mike Pall, *LuaJIT 3.0 New Garbage Collector* design notes (wiki,
  mirrored at github.com/tarantool/tarantool/wiki/LuaJIT-3.0-new-Garbage-
  Collector): arenas, cells, block/mark bitmap encoding + sweep word
  identities, bump/fit allocator, SSB, grey-stack-per-arena, huge-block
  side table, generational minor sweep, quad-color barrier discussion
  (we adopt the heap wholesale; barrier replaced per ADR-3).
- F. Pizlo, *FUGC* (fil-c.org/fugc; fil-c Manifesto.md; libpas fugc.c):
  on-the-fly grey-stack Dijkstra + soft-handshake fixpoint, phase-gated
  CAS-relaxed store barrier, allocate-black during mark, sweep-phase
  alloc-color rules — our marking protocol is this, retargeted.
- Doligez–Leroy (POPL'93) & Doligez–Gonthier (POPL'94): the original
  on-the-fly handshake collectors (DLG) — phase/handshake structure.
- Dijkstra et al., *On-the-Fly Garbage Collection: An Exercise in
  Cooperation* (CACM 1978): insertion barrier correctness frame.
- Yuasa (1990): SATB deletion barrier (fallback F-2).
- Go team, proposal 17503 *Eliminate STW stack re-scanning* (hybrid
  barrier): rejected-alternative F analysis.
- Chase & Lev, *Dynamic Circular Work-Stealing Deque* (SPAA'05): worker
  deques (05 §5.6.3) — use the corrected C11 formulation (Lê et al.,
  PPoPP'13 *Correct and Efficient Work-Stealing for Weak Memory Models*).
- D. Vyukov, bounded MPMC queue: channels (09 §9.5).
- T. Harris, *A Pragmatic Implementation of Non-Blocking Linked-Lists*
  (DISC'01): marked-pointer unlink for the string table sweep (06 §6.5.4).
- C. Click, lock-free hash table (JavaOne'07): state-machine/sentinel
  inspiration for KEYLOCK/FORWARD (06 §6.2–6.3; our variant is
  chain-based to preserve HREFK).
- Linux man-pages: membarrier(2) MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE
  (08 §8.5), memfd_create(2), futex(2).
- LuaJIT sources at the pinned commit (00 §0.2) — every cited
  file:line throughout docs 02–11.
- ntruessel/qcgc (github): independent implementation of the Pall arena
  GC — useful cross-check for bitmap edge cases.

# 02. The LuaJIT-MT Memory Model and the lj_atomic Layer

This document defines (a) what concurrent Lua programs may assume, (b) what
the C/asm implementation must do to deliver it, and (c) the exact atomics
API every other document builds on. `aux/lj_atomic.h` is the normative,
compilable form of §2.6 — copy it into `src/` unmodified at M1.

## 2.1 Language-level model (what Lua programmers get)

M-1 **Atomicity unit = one Lua value.** Reading a table slot, upvalue cell,
or any variable observable from another thread yields a value that *some*
thread wrote there (or nil if never written). No tearing, no out-of-thin-air
values, no type/payload mismatches. This follows from ADR-1: every shared
slot is one aligned 64-bit word accessed with atomic ops.

M-2 **Plain accesses are relaxed.** Without synchronization, threads may
observe each other's writes in different orders. `t.a=1; t.b=2` may be seen
as b-then-a elsewhere.

M-3 **Synchronizes-with edges** are created only by: `threading.channel`
send→recv (release→acquire on the transferred slot), `thread:join()`
(everything the dying thread did happens-before join returns),
`threading.spawn` (everything before spawn happens-before the new thread's
first instruction), and `threading.fence()` (seq_cst). Library docs in 09
restate this.

M-4 **Structural operations are individually linearizable but not
transactional.** A racing `t[k]=v` with `t[k2]=v2` is fine; iteration
(`pairs`) during concurrent mutation visits every key that was present for
the whole iteration, may or may not visit keys inserted/removed during it,
and never visits a key twice or crashes (06 §6.3.6). `#t` returns *a*
border. `table.sort` on a concurrently-mutated table gives unspecified
ordering but stays memory-safe.

M-5 **Memory safety is unconditional.** No data race — however perverse —
may cause a crash, type confusion, GC corruption, or reading of freed
memory. This is the runtime's burden, discharged by I-1..I-8 (01 §1.4).

M-6 **No guarantee of progress under contention is given to Lua code**, but
the runtime's own protocols are at least lock-free at the operation level:
some thread always completes (table ops, interning, allocation, barrier),
and no thread ever waits on a suspended peer except in explicitly blocking
APIs (join/recv).

## 2.2 What "lockless" means in this project (binding definition)

An operation is *hot-path lockless* iff its machine code contains no
mutex/futex acquisition and no unbounded spin that waits for a *specific*
other thread to act. Bounded helping (finish another thread's published
table migration step) and CAS-retry loops whose failure implies another
thread *completed the same kind of step* are allowed — that is the standard
lock-free progress condition. Blocking parks (futex) are allowed only in:
`ch:recv/send` on empty/full, `thread:join`, GC workers idling, finalizer
thread idling, and a mutator waiting for *its own* requested full GC
(`collectgarbage"collect"`). The handshake protocol itself never blocks a
mutator: mutators only ever *publish* acks (05 §5.4).

## 2.3 Hardware/compiler model targeted

- x86-64: TSO. Relaxed/acquire/release atomics compile to plain MOV; only
  seq_cst stores/fences emit `lock`/`mfence`-class instructions. Design
  accordingly: hot paths use relaxed/acq/rel exclusively.
- Compiler: GCC/Clang `__atomic_*` builtins on plain (non-`_Atomic`) types.
  This keeps struct layouts identical to upstream (critical: dasc and the
  JIT hardcode offsets) while giving defined concurrency semantics and TSAN
  visibility.

## 2.4 The TValue/GCRef accessor rewrite (mechanical, M1)

All shared-slot access in C funnels through a handful of macros in
lj_obj.h: `copyTV`, `setnilV`, `setboolV`, `setnumV`, `setintV`, `setgcV`,
`setgcref*`, `settabV`, … and reads via `gcval`, `tvis*`, `numV`, etc.
Redefine the *storage* macros to route the final 64-bit move through
`lj_atomic`:

```c
/* lj_obj.h, after TValue definition */
#define tv_rawload(o)      la_load64_rlx(&(o)->u64)
#define tv_rawload_acq(o)  la_load64_acq(&(o)->u64)
#define tv_rawstore(o,u)   la_store64_rlx(&(o)->u64,(u))
#define tv_rawstore_rel(o,u) la_store64_rel(&(o)->u64,(u))
#define copyTV(L,o1,o2)  tv_rawstore((o1), tv_rawload((o2)))
```
Notes:
- The interpreter's hand-written asm already moves TValues as single 8-byte
  MOVs (vm_x64.dasc throughout); on x86-64 that *is* a relaxed atomic
  access, so no dasc change is needed for I-1.
- Reads of a slot you might dereference as a pointer must be at least
  consume-ordered; on both targets a dependent load through the loaded
  pointer is naturally ordered, and we additionally use acquire loads at the
  few places that read a *structure header then its interior* without an
  address dependency (table gen header, string table vector — flagged
  individually in 04/05/06 as `la_*_acq`).
- Frame slots, `L->base..L->top`, and any TValue provably local to the
  owning thread (parser buffers, recorder slots, snapshot restore) keep
  plain accesses; the rule of thumb in code review: if another thread can
  name it, it goes through `tv_raw*`. Stack slots can only be named by their
  owner (cells replaced open-upvalue aliasing — ADR-7), so stacks are
  exempt: this is why interpreter stack traffic stays cost-free.

## 2.5 Field-by-field atomicity classification

Create `src/lj_mtfields.md` during M1 listing every struct field shared
across threads with its access class. Seed it with this table (extend as
you touch code):

| field | class | ordering |
|---|---|---|
| TValue in table array/hash, upvalue cell, registry, gcroot | shared slot | rlx (store: rlx; structural publish: rel) |
| GCtab.arrayhdr / nodehdr (new MRefs, 06) | RCU pointer | load acq / store rel |
| GCtab.metatable, GCudata.metatable | shared ptr | rlx; publish rel |
| GCtab.nomm | advisory byte | rlx (monotone-ish cache; safe to be stale) |
| GCobj.gch.gct | immutable after publish | plain (read), written pre-publish |
| arena bitmaps (block/mark) | atomic bitset | fetch_or rlx; sweep owns exclusive |
| g->gc2.phase | phase word | load rlx in fast paths, transitions rel + handshake |
| TG.poll / TG.reqmask | signal word | store rel by GC, load rlx by owner, ack CAS acq_rel |
| Node.key | write-once | CAS rlx claim, read rlx (re-checked, 06 §6.2.4) |
| Node.next | chain link | CAS rel insert, load acq walk |
| strtab bucket head | chain link | CAS rel insert, load acq walk; low bit = Harris mark |
| J->tracev / TraceVec.slot[i] | RCU vector + publish-once slots | vector store rel / load acq; slot store rel after mcode sync / load acq |
| J->retiredmcode | retired mcode records | CAS rel/acq; free after completed safepoint epoch |
| BCIns at patch sites | code word | single 32-bit store rel (`bc_publish`) |
| GCtrace.exittab[i] | retarget word | store rel; loaded by indirect-branch in mcode |
| L->thr_owner (new) | claim word | CAS acq_rel |
| g->str.tab vector ptr (becomes StrTabHdr*) | RCU pointer | acq/rel |
| cts->tab / cts->top | RCU vector + ticket | 11 §11.2 |

## 2.6 lj_atomic.h API (normative summary — full header in aux/)

Prefixed `la_`. All take plain-typed pointers.
```
u32/u64/ptr load/store in rlx|acq|rel|seq:  la_load32_rlx(p) ...
CAS (strong, value-returning):  la_cas32(p, &exp, des, succ_mo, fail_mo)
                                la_cas64 / la_casptr
fetch ops: la_add32_rlx, la_add64_rlx, la_or8_rlx, la_or64_rlx,
           la_and64_rlx, la_xchg32_acqrel, la_xchgptr_acqrel
fences:    la_fence_acq(), la_fence_rel(), la_fence_seq()
pause:     la_cpu_pause()            (PAUSE / ISB-free YIELD)
futex:     la_futex_wait(u32*p, u32 val, ns) / la_futex_wake(p, n)
process:   la_membarrier_synccore()  (wraps membarrier(2); 08 §8.5)
bit utils: la_bit_test_and_set64(p, idx) -> previous bit (fetch_or based)
```
Rules: no seq_cst on any per-operation hot path; every call site comments
which invariant (I-n or doc §) justifies its ordering.

## 2.7 TSAN posture

Because all C-side sharing flows through `__atomic` builtins, a
`-fsanitize=thread` build of the C parts is meaningful. The asm interpreter
is invisible to TSAN; therefore (a) the C unit-stress drivers in 13 §13.6
exercise tables/strings/GC/allocator directly from pthreads C code, and
(b) full-VM TSAN runs use `-DLUAJIT_DISABLE_JIT` plus the interpreter
suppression file `tsan.supp` (13 §13.6.3). A small set of
`__tsan_acquire/__tsan_release` annotations at asm↔C boundaries
(safepoint ack, channel transfer) is specified there.

# 12. Implementation Plan — Milestones M0..M10

Your task list. Each milestone: goal → tasks (with spec refs) → tests →
acceptance gate. Do not start Mn+1 before Mn's gate is green. Estimated
diff sizes are for sanity-checking scope, not deadlines.

## M0 — Harness & guardrails (≈300 lines)
Tasks: pin the commit (00 §0.2). Add the ADR-1 x86-64/GC64 `#error`
guardrail without creating a compatibility flag wall. CI scripts: build
matrix {-joff,-jon}; stock test suite runner (import
github.com/LuaJIT/LuaJIT-test-cleanup tests into tests/stock/); the greps:
`pthread_mutex` whitelist check (00 rule 3), `volatile` ban in new files,
`lj_gc_barrier` legacy-call detector (must hit 0 by M5 end). Run
`aux/bench/bench.lua` on your machine, both -joff/-jon, 5 runs, commit CSV as
`bench/baseline_<host>.csv`.
Gate: default builds pass the stock suite; bench CSV committed.

## M1 — Atomics + state split, behavior-neutral (≈1500 lines)
Tasks: drop in `aux/lj_atomic.h` as src/lj_atomic.h (verify it compiles
with gcc/clang on x86-64). Create lj_tg.h/.c with TGState (03
§3.2) embedded in GG_State; `g->jitp`; move tmpbuf/tmptv/tmptv2/prng/
cur_L/jit_base accessors through `G2TG`-style macros that resolve to the
embedded TG (sed worklists: `grep -rn "g->tmpbuf\|G(L)->tmpbuf" src` ≈40
sites; `grep -rn "g->cur_L\|->jit_base" src`). 02 §2.4 tv_rawstore macro
layer routes final 64-bit moves through `lj_atomic`. lj_mtfields.md seeded
(02 §2.5).
Gate: stock tests green; zero unintended asm diffs in vm_x64.o before the
dasc migration (objdump diff).

## M2 — dasc migration + allocator swap, still single-thread (≈3000)
Tasks: vm_x64.dasc TG addressing per 03 §3.5 dispositions A–F. lj_arena.{h,c}
per 04 (port aux/arena_bitmap_model.c verbatim for
bitmap/sweep/free-run code); retire lj_alloc.c body, keep low-address
mmap probing (04 §4.10); GCHeader change nextgc→gcw + gcflags (04 §4.7) —
this forces the lj_gc.c single-thread GC to switch sweep to arenas even in
the "legacy GC" path: DECIDED simpler: the legacy GC is no longer a
production object; M2 therefore brings up a degenerate gc2: STW-only mode
(leader runs mark+sweep with the world parked at a handshake — possible
with one thread trivially) so the system is collectable before M3 makes it
concurrent. The makefile object list moves from lj_gc.o/lj_alloc.o to
lj_gc2.o/lj_arena.o as that replacement lands.
Inline bump alloc in dasc (07 §7.5). Safepoint stub + poll sites compiled
but poll always 0 (07 §7.3).
Tests: stock suite (1 thread). Heap stress: tests/t-alloc-churn.lua
(alloc/free 10M mixed sizes, fragmentation walk). arena model asserts
already ran at M0 (it's standalone).
Gate: stock green; bench regression ≤8% at this stage (no barrier yet);
memory footprint within 1.3x of stock on t-alloc-churn.

## M3 — Concurrent GC, single mutator (≈4000)
Tasks: lj_safepoint.{h,c} full handshake machinery (05 §5.4); lj_gc2 full
(05 §5.3–5.13): mark/traverse ports (lj_gc.c functions listed in 05
§5.6.4 — port gc_traverse_* one by one), SSB, Chase-Lev deques, fixpoint
leader loop, lazy+worker sweep, defer_free epochs, pacing, torture,
LJ_GC2_PARANOIA STW-diff oracle (05 §5.13 — build this FIRST). Barrier:
add always-on C-side GC2 insertion hooks to the existing legacy barrier
macros first, preserving the legacy color barriers until their oracle role is
finished. Worklist for remaining direct/VM/JIT barrier owners:
`grep -rn "lj_gc_barrier\|lj_gc_objbarrier\|lj_gc_anybarriert\|
lj_gc_barrieruv\|lj_gc_barriert\b" src/*.c` (≈35 sites: lj_api.c lj_tab.c
lj_meta.c lj_func.c lj_state.c lj_cdata.c lj_ccallback.c lj_vmevent.c
lib_*.c) plus direct `lj_gc_barrieruv` VM/JIT call paths. dasc wbarrier_tv
macro + TSET*/USET* wiring (07 §7.4). Weak tables + finalizer queue minimal
(full in M8). collectgarbage mapping (05 §5.10).
Tests: stock under torture; paranoia build over the whole stock suite;
t-gc-01..06 (cycles under churn, weak basic, finalizer basic, resurrect,
huge objects, coroutine stacks).
Gate: paranoia oracle reports zero diffs across the suite; torture stock
green; bench (1 thread, GC concurrent) regression ≤10%.

## M4 — Threads exist: spawn/join/channels, JIT OFF (≈2500)
Tasks: lj_thr (pthread/futex shim), lib_threading + lj_chan per 09
(channel C unit test chan_stress.c FIRST, 13 §13.6.2); tg attach/detach
incl. mid-handshake attach (09 §9.3); thr_owner claims (06 §6.7, 03
§3.7); native-state enter/leave on blocking paths; HS_STOPREQ shutdown;
thread-activation latch (`global_State.mt_active`) + legacy-uv latch
behavior (10 §10.4, parser not yet changed — legacy machinery is what
exists, so the latch logic is exercisable immediately). This is runtime
state, not an `LJ_MT`/`LUAJIT_THREADSAFE` build gate.
Tests: t-api-01..10 (spawn/join/results/errors/timeout/close/rendezvous/
stress 1k threads sequential), t-mt-smoke (N threads pure-compute), 13
§13.3 litmus L1–L4 (message passing, join HB, fence SC, channel FIFO).
Gate: all green with `-joff`, 1..8 threads, under torture, 100 repetitions
of the litmus set; TSAN build of C unit drivers clean.

## M5 — Concurrent objects: tables/strings/cells (≈5000)
Tasks: tables per 06 §6.2–6.3 (port aux/nbtab_model.c; GCtab reshape;
IRFL offset constants updated even though JIT still off for MT); string
intern rewrite 06 §6.5 (+ sweep wave ordering); parser cell model +
CNEW/CGET/CSET (06 §6.4, 07 §7.6, 10 §10.2–10.3); bcread/bcwrite v4 (10
§10.1, 10.5, 10.6); long tail 06 §6.8; per-TG math.random.
Tests: stock (the parser change touches *everything* — expect a long
debug tail here; the v4-vs-v2 golden comparisons t-bc-01..03 and t-uv-01..
07 are your instruments); t-tab-01..08 (8-thread hammer suites: insert/
lookup/delete/resize/iterate/array-grow/mixed/len), t-str-01..03 (intern
storm, sweep-resurrect, resize race); nbtab_model + its in-tree port
share a fuzz seed corpus.
Gate: stock green (-joff); hammer suites 10-min soak
clean under torture; paranoia oracle still zero-diff; string identity
checker (intern 1M strings from 8 threads, assert pointer-equality per
content) passes.

## M6 — JIT on under the lockless runtime (≈4000)
Tasks: 08 in order: token (§8.2); registry RCU + bc_publish (§8.3–8.4.1);
exittab + assembler stub rows + trampoline (§8.4.2–8.4.3) — land the exit
indirection as a single-thread refactor first (stock JIT tests green + perf
delta ≤1%), then dual-map mcode (§8.5); XPOLL/XBAR IR + recording of
CGET/CSET (§8.6, §8.8); FLOAD hdr-indirection (§8.8.1); flush protocol
(§8.7).
Current implementation note: the original report's M6 task above remains the
canonical target. The current x86-64 bridge has implemented guarded `IR_XPOLL`
for LOOP-backedge trace safepoint polls and inlined FUNCF-depth entries. The
first TGMARK invalidation slice keeps `TBAR`/`OBAR` inside XPOLL-delimited
poll regions; broader XBAR invalidation work remains pending.
Tests: stock with -jon; t-jit-01..06 (trace same loop from 2 threads;
side-trace attach while parent runs on another thread; flush storm;
exit-handler stress; recording-thread killed mid-trace (error in
recorder); stitching). Scaling: bench_mt.lua first real numbers.
Gate: stock -jon green; single-thread JIT bench regression
≤10% geomean vs M0 CSV (THE gate of the project); t-jit suite 10-min soak.

## M7 — FFI (≈2000)
Tasks: 11 §11.2–11.7. Order: CTState RCU (unit-test ctype_stress.c),
cdata alloc switch, ffi.gc registry, native-state wrapping + fast-call
exemption, callbacks attach, clib.
Tests: t-ffi-01..06 (11 §11.8); stock FFI tests under torture, 8 threads.
Gate: green; ffi_struct bench within 10% of M0.

## M8 — Weak tables & finalizers, complete semantics (≈1200)
Tasks: full gc_mayclear rule port (05 §5.8), resurrection-race store hook,
finalizer thread + ordering, ffi.gc interplay, __gc on tables? (5.1: only
udata/cdata — keep), lua_close finalization drain (09 §9.6).
Tests: t-weak-01..05 (k/v/kv modes, resurrection, cycle of weak tables,
finalizer ordering, finalizer that spawns a thread).
Gate: paranoia oracle extended to weak sets, zero-diff.

Current implementation note: the original M8 target above remains intact.
`tools/ci/m8_weak.sh` now covers deterministic weak-mode semantics,
string weak-reference parity, weak-table cycles, current `ffi.gc()` ordering
and clear/nested-GC behavior, close-time cdata/userdata finalizer drain, and
focused GC2 weak/barrier paranoia coverage. It also reruns the GC2 phase
accounting test to assert finalizer owner tracking and enter/leave counter
balance under the current bridge. The traversal gate now covers
`lj_gc2_weak_complete()` skip/fallback accounting for the current
GC2-cleared snapshot bridge and a post-clear weak-phase store hook for the
original resurrection-race case. Paranoia builds now rescan the legacy weak
list after a GC2 skip decision and fail if any weak slot remains clearable.
Finalizer dispatch now has a GC2 owner try-claim around legacy
`mmudata` draining, so peer TGs back off instead of racing the shared finalizer
list while close-time drains still complete through the blocking wrapper. The
older `finalizer_token` bridge has been removed; the remaining finalizer bridge
is the shared `vmthread(g)` callback stack.
The original "finalizer that spawns a thread" item now has a bridge test for
spawn+join during explicit-GC finalization; the broader planned async finalizer
dispatch path remains M8 work, not an M9 performance cleanup.

## M9 — Performance closing (open-ended; budget ≈2000)
Menu (in expected-value order; measure each): per-arena grey stacks +
priority queue (05 §5.6.3); inline-alloc IR on trace (08 §8.6); cell
sinking (08 §8.8.4); bump-window policy tuning (04 §4.4); barrier
hoisting quality (XBARFLG CSE region sizing); TG layout cache-line audit
(perf c2c); channel spin K tuning; sweep SIMD (the bitmap identities
vectorize — 04 §4.1.1).
Gate: ≤10% single-thread geomean (stretch 5%); bench_mt scaling ≥6x on
8 cores for tab_hash_read-MT and arith-MT; GC pause P99 (mutator-observed
via poll latency probe) <500µs on the churn bench.

## M10 — Generational mode (≈800)
Tasks: 05 §5.12 (minor sweep identity already in arena code from M2;
remembered-set SSB mode; heuristic switch; collectgarbage("generational")).
Gate: alloc_tables bench ≥1.5x vs M9 full-cycle mode; paranoia (major
after minor) zero-diff.

## 12.1 Per-file change index (cross-check before declaring any milestone done)
new: lj_atomic.h lj_tg.{h,c} lj_arena.{h,c} lj_gc2.{h,c} lj_gc2_barrier.h
lj_safepoint.{h,c} lj_thr.{h,c} lj_chan.{h,c} lib_threading.c
lj_exittab.{h,c} tsan.supp tests/* bench/*
modified (grep-verify each against its spec doc): lj_obj.h(02,03,04,06)
lj_arch.h(01) lj_def.h lj_gc.{h,c}(05: retired/replaced by lj_gc2) lj_alloc.{h,c}(04)
lj_tab.{h,c}(06) lj_str.{h,c}(06) lj_buf.{h,c}(06) lj_func.{h,c}(06,10)
lj_state.c(03,09) lj_api.c(05,06,09) lj_meta.c lj_debug.c(06)
lj_dispatch.{h,c}(03,08) lj_parse.c(06,10) lj_bcread.c lj_bcwrite.c(10)
lj_bc.h(10) lj_record.c lj_ffrecord.c(08) lj_ir.h lj_opt_fold.c
lj_opt_mem.c(08) lj_asm.c lj_asm_x86.h lj_emit_x86.h(08)
lj_trace.c(08) lj_mcode.c(08) lj_snap.c(08,10) lj_gdbjit.c(08)
lj_ctype.{h,c} lj_cdata.{h,c} lj_ccall.c lj_ccallback.c lj_cparse.c
lj_clib.c(11) lib_base.c(05 collectgarbage) lib_ffi.c(11) lib_init.c(09)
lib_package.c(06 §6.8) vm_x64.dasc Makefile
Makefile.dep host/genlibbc? (only if new lib uses LJLIB macros — it does:
regenerate buildvm output).

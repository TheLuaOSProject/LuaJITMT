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

Current implementation note: `collectgarbage("workers", N)` exposes the staged
capped parked GC2 worker pool: missing `N` queries the current count, `N <= 0`
stops it, and positive `N` starts up to the current cap while returning the
previous count. Worker lifecycle changes are serialized by
`GC2State.worker_control`, with dead registry-visible worker TGs retired until
TG reclamation can unlink them; the full per-worker-deque pool remains the
original M3/M9 scheduler target.

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
poll regions; broader XBAR invalidation work remains pending. The CGET/CSET
recording subtask is covered for the current x64 bridge by explicit IR guards
over raw-slot fallback, promoted-cell `UREFC`/`ULOAD`/`USTORE`, and GC-valued
`OBAR`; the first `BC_CNEW`/`BC_FNEW` helper-backed slice covers source and
loaded v4 self-cell creation traces and removes the CNEW-specific `PROTO_NOJIT`
gates. Mixed raw-local FNEW traces are now covered for source/loaded immutable
raw captures through stack-value synchronization, mutable captures once the
cell is promoted at trace entry, and source/loaded first-promotion loops where
the hot trace performs the first mutable raw-slot promotion with otherwise
type-stable loop slots. A narrow helper-backed table-store bridge now records
in-bounds `ASTORE`/`HSTORE` updates on Linux/x64, including existing non-nil
slots, previous-nil slots, trace-local new string-key hash slots, and
trace-local new numeric slots, plus shared, PHI-carried, upvalue-carried,
escaped, and weak-table references. These lower through split hash/array/newref
helpers that run the parent-aware value barrier and the P_WEAK weak-write
bridge; M10 now also pins that traced helper-backed array stores enqueue the
old parent, not the young value, for idle generational remembered-set draining.
The x64 VM `TSETV`/`TSETB`/`TSETR`/`TSETM` fast array/range stores now use
post-store publication helpers instead of pre-store black-table repair retry
branches, preserving the `TSETM` table barrier while the stored value snapshot
drives GC2 and legacy repair.
The original generation-aware table write protocol and broader
AHdr/NHdr table-generation port remain pending.
Linux/x64 HREFK recording now avoids the legacy `GCtab.hmask` mirror
guard and relies on the loaded node pointer plus x64 node-header slot guard;
empty-hash misses fall through to regular `HREF` instead of the legacy
`TAB_HMASK == 0` shortcut. Shared array reads now have an interim x64
pair-stability guard that brackets the `TAB_ASIZE` bounds check with two fresh
`TAB_ARRAY` loads and guards the array pointers equal before `AREF`/`ALOAD`.
Separated shared array reads now use `TabArrayHdr.asize` loaded through the
single recorded `GCtab.array` slots pointer; colocated shared arrays keep the
legacy pair guard. The header path checks the observed slots pointer so a
split-from-colocated publication window does not treat colocated storage as a
header-backed allocation, and runtime guards exit for `NULL`/colocated array
pointers before the header load. Separated visible-size changes now publish
fresh array-header generations; shrink generations preserve capacity for stale
mirror readers but nil hidden tail slots. GC/GC2 traversal, table-size
accounting, and table free now derive separated-array capacity from the array
header snapshot rather than `GCtab.acap`. This leaves the broader
table-generation model pending around legacy mirrors, remaining owner-side
C mirror readers, and helper-backed stores.
Helper-backed Linux/x64 `ASTORE`/`HSTORE` stores now pass the array index or
hash key into their helpers and resolve before CAS if the recorded slot is
outside the current generation; `NEWREF` stores key-resolve before each CAS.
This closes the stale retiring-generation slot case for helper-backed traced
table stores while the broader generated RETIRING/FORWARD/CAS write protocol
remains pending.
Linux/x64
secure builds now use the original M6 dual-map mcode write view: each mcode
area is memfd-backed, mapped once RX and once RW, `MCLink.rw` carries the
writable alias, generated-code/unwind writes go through RX/RW translation
helpers, and teardown unmaps both aliases. The M9 cleanup removed the
conservative fresh-area bridge; reserve now reuses the current dual-map area
through its RW alias after publication, and new areas are allocated only on
normal mcode exhaustion. Recorder-internal `LJ_TRLINK_RETURN` call-unroll flushes now
use `lj_trace_flushscope()`, sharing the public scoped `HS_EXIT_TRACES` boundary
and slot-retirement path instead of leaving marked scoped slots behind. Numeric
side-trace flushes now mark and retire the named side slot through the same
boundary while preserving the root trace.
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

Current implementation note: the original M7 target above remains intact.
FINREG uses CAS-published weak-key generations for cdata finalizer registry
growth, and recorded `ffi.gc()`/ctype `__gc` now emits the FINREG mutation
helper instead of falling back to NYI. The current guard covers direct
registration, nil clear, metatype registration, and multi-threaded default-JIT
FINREG stress; the broader final FINREG/finqueue execution design remains M8
follow-up rather than M9 performance cleanup. x64 callback runtime scratch is
per TG; callback entry chooses the current TLS carrier for attached callers and
auto-attaches a hidden callback carrier for TLS-less foreign pthread entry.
Callback slot claiming creates that hidden carrier only after observing a free
owner slot, so slot-table overflow stays on the existing error path without
allocating an unclaimable carrier.

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
`lj_gc2_weak_complete()` skip/backfill/fallback accounting for the current
GC2-cleared snapshot bridge, captured traversal-time weak modes across later
`mt.__mode` mutation in both drain and late-write barriers, and a post-clear
weak-phase store hook for the original resurrection-race case, including VM
rewrites of existing nil-valued weak-key hash slots and VM insertion of strong
hash keys into weak-value tables. VM table-valued `__newindex` stores now use
the resolved owner for weak-value late-write marking after the slot CAS.
`table.insert()` direct array writes now use the same weak-write bridge for
weak-value tables during `P_WEAK`, and direct C API raw setters are covered for
all-weak hash and weak-value array writes.
The weak-clear bridge now treats a current-cycle GC2 mark from a pre-drain
late write as authoritative over stale legacy white during `GCSatomic`, keeping
the original weak-table semantics target while avoiding a larger plan change.
Paranoia builds now rescan the legacy weak list after a GC2 skip decision and
fail if any weak slot remains clearable.
Finalizer dispatch now has a GC2 owner try-claim around legacy
finalizer draining, so peer TGs back off instead of racing the shared finalizer
state while close-time drains still complete through the blocking wrapper. The
older `finalizer_token` bridge has been removed, and normal finalizer producers
publish through the GC2 MPSC/consumer-ring bridge instead of legacy `mmudata`;
that bridge now links dedicated queue nodes instead of overwriting each pending
object's `gcw` root/list link. Dispatch-time queue consumption goes through
`lj_gc2_finalizer_dispatch_one()`, leaving legacy GC with the dequeued-object
semantic callback rather than direct owner/drain/dequeue access.
User finalizer callbacks now run on the claimed collector caller `lua_State`
instead of the shared `vmthread(g)` stack. Userdata FINREG membership now uses a
GC2 side list for discovery, including in-place metatable finalizer additions
and manually chain-unlinked userdata; `lj_gc2_finreg_udata_finalize()` owns the
side-list walk and finalizer queue publication, while
`lj_gc2_finreg_udata_dispatch()` owns dispatch-time FINREG clear and `__gc`
lookup before calling back to the legacy protected callback runner. Cdata
ordered FINREG discovery covers the ordinary P_WEAK and close-time paths, and
`lj_gc2_finreg_cdata_dispatch()` owns dispatch-time FINREG slot/preclaim
resolution before calling back to the legacy protected callback runner. GC2
also owns close-time FINREG generation disable through
`lj_gc2_finreg_cdata_disable()`. The M9 cleanup removed close-time
generation-table pending/discovery scans; P_WEAK preclaim side-vector failure
now restores and queues the same ordered object without a legacy root-list walk.
The GC2 FINREG root helper layer owns the cdata FINREG generation/preclaim root
walks: legacy GC enters through `lj_gc2_finreg_cdata_mark_roots()`, and GC2 keeps
preclaims in pending roots while marking FINREG generations from the FFI global
root path. Collector-specific callbacks preserve each marker's coloring
semantics.
Queued finalizer marking now routes through `lj_gc2_finalizer_mark_all()`, so
GC2 owns the finalizer owner claim, producer-stack drain, and stable ring walk
while legacy marking callbacks preserve classic collector coloring semantics.
If cdata with a live finalizer reaches sweep/free, that is now a fatal FINREG
invariant instead of a rescue queue; safety and Lua finalizer semantics win over
trying to recover from a missed owner edge.
The broader planned FINREG/finqueue dispatch path remains M8 work rather than
an M9 performance cleanup.
The original "finalizer that spawns a thread" item now has bridge tests for
spawn+join during explicit-GC finalization and for a worker that outlives the
callback; the latter defers the full-GC loop back to the mutator instead of
waiting forever on `mt_live`, and explicit `collectgarbage("step", ...)` no
longer reports cycle completion while that spawned worker is still live. The M8
gate also runs a reduced `t-ffi-gc-finreg.lua` smoke so worker-created FINREG
finalizers must fire exactly once under interpreter and default-JIT modes. The
broader planned async finalizer dispatch path remains M8 work, not an M9
performance cleanup.

## M9 — Performance closing (open-ended; budget ≈2000)
Menu (in expected-value order; measure each): per-arena grey stacks +
priority queue (05 §5.6.3); inline-alloc IR on trace (08 §8.6); cell
sinking (08 §8.8.4); bump-window policy tuning (04 §4.4); barrier
hoisting quality (XBARFLG CSE region sizing); TG layout cache-line audit
(perf c2c); bridge cleanup for temporary tokens/owner claims/locks that are
not part of the final design, especially leftover temporary locks and tokens;
channel spin K tuning; sweep SIMD (the bitmap identities vectorize — 04 §4.1.1).
Accepted serialized surfaces are `ffi.cdef`, `require`, and the serialized
recorder/recorder token; do not spend M9 trying to make those parallel unless a
correctness issue demands it.
Gate: ≤10% single-thread geomean (stretch 5%); bench_mt scaling ≥6x on
8 cores for tab_read-shared and arith-MT; GC pause P99 (owner-side,
mutator-observed poll acknowledgements) <500µs on the churn bench.

Current implementation note: `collectgarbage("stats")` now returns a
benchmark-facing table of GC2 counters for cycle starts, requested vs actual
minor cycles, allocation pacing, poll-ack latency, assist/worker progress, owner
sweep progress, weak clearing, weak-write marks, FINREG fallback/order counters,
finalizer queueing/MPSC drains, finalizer-spawn deferrals, and live estimates.
`plan/aux/bench/bench_mt.lua` prints a stable subset of those fields after a run
and reports approximate owner-side poll-ack P99 latency from histogram deltas;
synthetic leader and remote-native acknowledgements do not contribute to the
poll-latency histogram.
`tools/ci/m9_m10_gc.sh` now aggregates the M9 stats smoke, M9 benchmark smoke,
and M10 generational guard, and each child guard checks that aggregate wiring.

## M10 — Generational mode (≈800)
Tasks: 05 §5.12 (minor sweep identity already in arena code from M2;
remembered-set SSB mode; heuristic switch; collectgarbage("generational")).
Gate: alloc_tables bench ≥1.5x vs M9 full-cycle mode; paranoia (major
after minor) zero-diff.

Current implementation note: the public `collectgarbage("generational")` /
`collectgarbage("incremental")` mode toggle now drives a passive
`GC2State.generational` bit and exposes it through `collectgarbage("stats")`.
Full GC now sets a one-shot major override, and generational mark begins record
minor-cycle requests through `minor_cycle_requests`; fully minor starts are
counted separately by `minor_cycle_starts` once the minor sweep/root gates latch.
Idle generational barriers conservatively queue remembered entries into SSB
without draining outside a cycle and force a major on overflow. Minor sweep and
root gates turn on after the first forced major baseline. Completed minor cycles
now estimate young survival from live-estimate growth over sampled cycle
allocation bytes and request a one-shot major at high survival. `bench.lua`
accepts `BENCH_GC_MODE` and `BENCH_SCALE` for M10 tuning probes; the initial 80
percent survival threshold leaves short-lived allocation churn on minor cycles
and promotes retained-allocation churn back to major collection. The M10 C
harness now exercises both interpreted VM-fast and traced helper-backed table
stores under idle generational mode and verifies the remembered entry is the old
parent table before the next minor cycle marks the young child.

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

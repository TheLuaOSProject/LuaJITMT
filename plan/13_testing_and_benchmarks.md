# 13. Testing & Benchmarks

## 13.1 Test taxonomy
1. **Stock suite** (imported, M0): semantic ground truth for the default
   lockless build.
2. **Conformance** tests/t-*.lua: new semantics (threading API, cells,
   bytecode compat, weak/finalizers) — listed throughout docs 06–11.
3. **Litmus** (§13.3): memory-model edges, run 100–10k reps.
4. **Hammer/stress** (§13.4): hours-scale soak under torture.
5. **C unit drivers** (§13.6.2): lock-free structures isolated, TSAN-clean.
6. **Oracles**: LJ_GC2_PARANOIA STW diff (05 §5.13); string identity
   checker; bytecode golden files.
7. **Fuzzing** (§13.7).

## 13.2 Baseline numbers (reference machine)

Recorded from the pinned commit, container: 1 vCPU Intel Xeon @2.80 GHz,
3.9 GiB RAM, gcc 13.3.0, Linux x86-64, default build (GC64 on). Best of 5,
`aux/bench/bench.lua`. **Single-vCPU machine: these are single-thread
baselines only; redo scaling runs (bench_mt) on ≥8 real cores.**
Use `BENCH_GC_MODE=generational|incremental` to pin the GC mode for a run, and
`BENCH_SCALE=<factor>` for short tuning probes; unset both for canonical
baseline numbers.

| benchmark | JIT ns/op | interp ns/op |
|---|---|---|
| arith_loop | 1.22 | 4.95 |
| fib30 (total) | 11.57 ms | 45.84 ms |
| tab_hash_write | 35.51 | 74.84 |
| tab_hash_read | 32.21 | 76.21 |
| tab_array | 3.94 | 19.13 |
| alloc_tables | 1.21 | 71.37 |
| string_intern | 103.54 | 156.17 |
| closures_upval | 95.63 | 109.78 |
| upval_hot | 1.21 | 13.71 |
| ffi_struct | 1.21 | 141.13 |
| coroutine_switch | 47.74 | 37.35 |
| sbuf_format | 113.72 | 161.92 |

Gate math (M6/M9): geomean over the JIT column of (new/old) ≤ 1.10.
Watch-list pairs (the design's known taxes → the benchmark that catches
them): barrier ⇒ tab_array & tab_hash_write; cells ⇒ closures_upval &
upval_hot; gen-header indirection ⇒ tab_hash_read; intern CAS ⇒
string_intern; native-state ⇒ ffi_struct; poll ⇒ arith_loop & fib30;
TG tmpbuf ⇒ sbuf_format.

## 13.3 Litmus tests (tests/litmus/*.lua, 100+ reps each, assert exact)
L1 message passing: t1 `x.v=42; ch:send(1)`; t2 `ch:recv(); assert(x.v==42)`.
L2 join HB: child writes 1000 table keys; parent joins; asserts all.
L3 fence SC (Dekker): two threads store-flag/fence/load-other; assert not
   both saw 0 (statistical: any violation fails).
L4 channel FIFO per producer: 2 producers × 10k tagged sends, consumer
   asserts per-producer order.
L5 cell visibility: 09 §9.11 example 2 formalized.
L6 no-tear: t1 alternates t.k between two GC values; t2 reads 1M times,
   asserts every read is one of the two exact references (catches torn
   tags vs payload).
L7 init-publish: t1 builds a 100-key table then publishes via channel; t2
   asserts all keys (acquire edge covers interior).

## 13.4 Hammer suites (tests/stress/)
t-tab-01..08, t-str-01..03 (12 §M5), t-gc-01..06 (M3), t-jit-01..06 (M6),
t-ffi-01..06 (11 §11.8), t-weak-01..05 (M8), t-uv-01..07, t-bc-01..03,
t-api-01..10. Plus combined: stress/kitchen.lua — N threads each running a
random mix (tables, strings, closures, channels, coroutines, ffi, GC
pokes) from a seeded PRNG for T minutes; any error/assert/crash fails;
run under torture and normal pacing; this is the long-haul soak.
Coroutine×thread matrix: t-co-01 resume/yield storms while markers scan
(exercises thr_owner GCSCAN spin); t-co-02 cross-thread resume handoff of
the same coroutine through a channel.
Current M6 scaffold gates include dispatch redispatch, recorder-token
ownership, local-cell recorder IR shape including self-cell CNEW/FNEW creation,
mixed raw-local FNEW sync-helper traces, promoted-cell update loops,
first-promotion FNEW traces, XPOLL barriers, XBAR/XPOLL aliasing, allocation
accounting, TNEW/CNEW/SNEW GC2 hard-check readiness, shared AREF
pair-stability, GC-step bridging, mcode publication, public and
recorder-internal scoped flush handshake coverage, and numeric side-trace
flush slot-retirement coverage. The table-store helper gate also rejects
same-trace closed-upvalue and nested heap escapes before a `TNEW`/`TDUP` slot
update.
These are milestone
guardrails, not the final M9 performance matrix.

## 13.5 GC-specific unit tests (C, tests/c/)
gc2_fixpoint_test.c: detector unit (05 §5.7.1) — mock workers inject
marks; assert termination exactly when a round is clean.
arena_sweep_test.c: extends the aux model with randomized alloc/mark/sweep
cycles, asserts live-set preservation + free-coalescing (port from
aux/arena_bitmap_model.c main()).
defer_free_test.c: epoch grace — retire under reader load, ASAN build,
assert no UAF and no leak (counted).

## 13.6 Sanitizers
### 13.6.1 ASAN/UBSAN
Full builds, all Lua suites, both -joff/-jon (use the explicit insecure mcode
flag only if an ASAN environment cannot tolerate W^X protection flips). Arena
allocator gains ASAN poisoning hooks: poison free runs,
unpoison on alloc (ASAN_(UN)POISON_MEMORY_REGION; ~20 lines, dev builds).
### 13.6.2 TSAN C unit drivers (the load-bearing TSAN coverage)
tests/c/{nbtab,strtab,chan,deque,arena,safepoint}_stress.c — pthreads
drivers hitting the in-tree implementations directly (not via the
interpreter): N threads × M ops, invariant checks. These MUST be
TSAN-clean with zero suppressions; they are built/run in CI every commit
from their milestone onward.
### 13.6.3 Full-VM TSAN
`make TSAN=1` ⇒ -fsanitize=thread -DLUAJIT_DISABLE_JIT plus tsan.supp
containing only: the interpreter object (called_from_lib:lj_vm*), and the
two asm↔C boundary races TSAN cannot see through (document each entry).
Add `__tsan_acquire/__tsan_release` annotations: lj_safepoint_ack (acquire
reqmask / release hs_pending), channel send/recv slot edges (mirror the
la_ ops so TSAN models them when called from C), thr_owner claim.
Run litmus + t-api + t-tab suites under it weekly-equivalent cadence.

## 13.7 Fuzzing
- luaL_loadbuffer fuzz (bcread v2/v3/v4 verifier, 10 §10.5): libFuzzer
  harness fuzz/fuzz_bcread.c, corpus seeded with stock dumps + v4 dumps.
- table-op sequence fuzzer: fuzz/fuzz_tabops.c drives the C table API with
  an interpreted op-string across 2–4 threads, shadow-checked against a
  per-key last-writer-wins oracle where determinable (single-writer keys).
- ffi.cdef grammar fuzz reused from upstream practice (cparse).

## 13.8 Benchmarks: multi-thread suite (aux/bench/bench_mt.lua)
Scaling curves 1,2,4,8 threads: arith-MT (embarrassingly parallel),
tab_hash_read-MT shared table, tab_hash_write-MT sharded vs shared,
alloc-MT (allocator scalability — the headline number for ADR-4),
string_intern-MT (worst-case shared structure), chan_pingpong (latency),
chan_throughput, pmap-image-kernel (the 09 §9.11 pmap on a synthetic
workload). Report ops/sec/thread + total; plot speedup. GC metrics are dumped
via `collectgarbage("stats")`: current fields include cycle requests/starts,
poll-ack sample/sum/max latency plus histogram buckets for approximate P99,
allocation trigger/hard-limit bytes, assist work, worker work, owner sweep
work, weak clearing, finalizer queueing, and live estimates. `bench_mt.lua`
reports per-run poll-ack P99 bucket bounds from histogram deltas.

## 13.9 CI matrix (final)
{x64} × {-joff,-jon} × {release, ASAN, TSAN-C, paranoia,
torture-soak(nightly)} — gating per 12's milestone gates.

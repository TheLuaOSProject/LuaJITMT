# Implementation and architecture review

Scope: LuaJITMT ARM64 branch at `a907dbb9`, with the rooted numeric-read change
reviewed and integrated at `0d9475d8`. This is a source review and
targeted execution study, not a proof of all runtime interleavings. After
integration, the full experimental gate, both ARM64 stock 509-case corpora,
and the complete bootstrap gate passed sequentially on frozen `d8f02a81`.
All default target branches ran; the bootstrap suite includes 387 stock cases.
The same frozen code also passed both 509-case stock corpora on x86-64/Rosetta,
the registered errno case, and the nested-universe/foreign-actor fixtures with
matched assertion/helper profiles and signed executables.

## Recommended direction

Keep the ownership, publication, and reclamation architecture. Change the way
the port grows: certify reusable operations and effects, move immutable work
out of repeated entry checks, and make native coverage and progress visible.
Adding another whole-trace pattern for every example will not converge quickly
to a general LuaJIT backend.

Do not enable the old ARM64 raw table emitters wholesale. Their `HREF`,
`HREFK`, `ALOAD`/`HLOAD`, and store paths do not acquire the same lifetime and
generation authorities used by the shared runtime. The current fail-closed
boundary prevents those differences from becoming live memory-safety defects.

## Priorities and evidence

| Priority | Observation | Recommended change | Required evidence |
| --- | --- | --- | --- |
| 1 | `lj_trace_enter_root()` calls the full allocated-layout validator twice per entry, although published IR and layout are immutable under its lifetime lease. | Remove the second scan while retaining view equality and every mutable entry check. Then evaluate an immutable certificate published only after both independent validators pass. | Existing corrupted-layout, flush/reuse, slot, bytecode, PAC, gate, and child-topology tests; matching-build entry benchmarks. |
| 1 | Ordinary INT-to-NUM specialization can leave an ARM64 loop repeatedly taking guards while general side traces remain unavailable. Correct answers and a trace count conceal this. | Make side-exit frequency and actual native work part of acceptance; prioritize numeric type adaptation and general scalar side-entry support. | INT and NUM initial values, changing types, overflows, short and long loops, concurrent GC/flush, and exit/entry counts. |
| 2 | Root, side, and FFI admission still contain exact bytecode lengths, IR ordinals, register assignments, and selected exit numbers. | Replace these incrementally with operation/effect contracts: scalar arithmetic, rooted no-wait reads, rooted stores, allocation publication, call ABI, and snapshot continuation. | Independent semantic and post-RA implementations, corrupted-graph negatives, generated code, and lifecycle tests for each contract. |
| 2 | Native FFI admission covers three exact signatures; arbitrary arm64e generated-frame unwind still has only synthetic evidence. | Grow admission by ABI argument/result classes using the existing metadata-snapshot discipline, and add real generated Lua/native/callback unwind coverage before widening the promise. | Register and stack classes, aggregates/sret, variadics, result roots, errno, callbacks, C++ exceptions, and remote GC/flush. |
| 2 | Many shell contracts duplicate builds, profile restoration, and checks for source text. Several stale text checks blocked valid code during this session. | Centralize build/run plumbing and an explicit profile identity; retain independent semantic/runtime assertions. Replace text checks with compiled or emitted-object checks where those prove the intended property. | All registered cases execute, archives and fixture translation units match, native branches cannot silently skip, and negative controls still fail. |
| 3 | A scalar table hit can retain table, key, array, and node bodies separately. The global rescan pending credit is also shared across tables. | Measure lease admissions, retries, and pending-credit contention before specializing or sharding. A numeric-index fast path is a candidate, not a demonstrated improvement. | Hits, misses, resize, retirement, contention, throughput, tail latency, and generated atomics on both targets. |
| 3 | The code uses bounded try operations, retries, cooperative polls, and scheduler waits for different purposes. | Document each operation's progress contract and allowed suspension points. Test stalled publishers and collectors explicitly. | Bounded refusal for try helpers; progress or a documented external dependency for retrying operations. |

### Entry validation: immutable versus mutable

The first `trace_root_entry_arm64_layout_valid()` checks IR, constants, register
and spill layout, snapshots, and exit geometry. `trace_root_entry_loop_view_equal()`
compares the reacquired body pointers, counts, and identity. The second view
also rechecks slot/vector identity, retirement/runnability, root/child topology,
and bytecode. PAC and pending requests are checked again before dispatch.

Published snapshot `count` is mutable, but the post-RA layout checker does not
consume that field. The optimization must preserve the other checks and the
TG's published `jit_base` lifetime lease. A future certificate must bind to the
exact body/incarnation and code identity; a global boolean saying that some
trace was once valid would be insufficient.

Independent semantic and post-RA validation remains valuable. Share mechanical
types and build plumbing where useful; do not make one validator accept merely
because the other did, or generate both verdicts from one unchecked table.

### Measured entry and specialization behavior

Exploratory measurements on Apple M5 Max, macOS 27.0 (26A428), frozen `d8f6ab68`.
The build had assertions, `LJ_ARM64_EMIT_TEST_HELPERS`, and
`LJ_TRACE_TEST_HELPERS`; these are not release-performance claims. Each number
is median process CPU time over five rounds. The driver was interpreted and
`texit` observation was disabled during timing.

| Loop | Calls per round | Interpreter seconds | JIT seconds | Native warmup exits / 20 calls |
| --- | ---: | ---: | ---: | ---: |
| 8 iterations, NUM initial accumulator | 100,000 | 0.018442 | 0.056093 | 20 |
| 1,024 iterations, NUM initial accumulator | 10,000 | 0.141897 | 0.012171 | 20 |
| 1,024 iterations, INT initial accumulator | 10,000 | 0.145688 | 5.038312 | 20,460 |

The last case computes the correct result. Its trace dump shows an INT entry
guard and checked NUM-to-INT conversion repeatedly exiting. The probe set
`hotexit=1000`, which suppresses ordinary side recording; its exit count alone
does not establish whether a normal hot-exit threshold can adapt. The count
does demonstrate the cost of repeatedly entering that incompatible root.
Numeric adaptation requires a separate test that observes a real child trace,
its parent/exit relationship, and reduced exit frequency. Two published root
traces are not evidence of a side trace. The first two measurements show
entry-dominated versus sustained work; they do not isolate validator cost by
themselves.

Raw logs are `/tmp/luajitmt-entry-review-{stable-,}20261025.log`; the original
probe used a literal initial accumulator. The checked-in
[`bench/arm64-entry-cost.lua`](../bench/arm64-entry-cost.lua) permits the initial
value as an argument and emits that literal into the kernel. For example:

```sh
src/luajit bench/arm64-entry-cost.lua off 8 100000
src/luajit bench/arm64-entry-cost.lua on 8 100000
src/luajit bench/arm64-entry-cost.lua on 1024 10000 0
```

The duplicate-scan change passed the complete focused root-entry contract on
ARM64 and arm64e, including side ingress and corruption/request races. A
matching clean build reduced the stable 8-iteration JIT median to
0.038658–0.039655 seconds (about 29–31%); the interpreter controls were
0.018144–0.018313. The 1,024-iteration JIT median was 0.010601–0.011059,
with interpreter controls 0.141901–0.145211. Each JIT warmup still had one
trace and 20 exits. These are instrumented-build measurements; no general
release-speedup or whole-port parity claim follows.

### GC2 and table state

The sampled close/rescan protocol does not reveal a new correctness defect.
The per-table INSTALLING/COUNTED/CANCELLED state, NEEDSCAN hint, global pending
credit, and concrete SSB item serve different purposes. Removing one because
it looks redundant would erase either ownership, an exact close obligation,
or the identity of work that must survive into the next cycle.

The terminal sweep-close gate is also distinct from the generic GCSCAN gate.
The latter can forward work during an active collector phase. Treating both
as a reason to drop counted publication was an invalid simplification and was
rejected before integration. Keep a compact phase/ownership transition table
alongside the deterministic close-race tests.

`lj_tab_getscalar_rooted_try()` makes no scheduler wait and retains exact body
authorities. Other table readers and safepoint/attachment paths do retry or
wait. A mutex-free implementation is not, by that fact alone, a proof that an
entire operation is wait-free or that a stalled participant cannot delay it.
Use the actual per-operation progress contract when evaluating the design.

Inspection anchors: `src/lj_tab.c:lj_tab_getscalar_rooted_try`,
`src/lj_gc2.c:lj_gc2_small_lease_try` and `lj_gc2_sweep_to_idle`, and
`src/lj_safepoint.c`'s acknowledgement and native-root completion paths.
These are optimization candidates with specific invariants, not a recommendation
to replace the collector or broadly weaken memory ordering.

### Test reliability and maintainability

The new admission fixture initially copied `[0, nins)` from a biased trace IR
pointer. Only `[nk, nins)` belongs to that allocation; address-layout changes
made the overread intermittently fault. All copied trace views now use the
valid range. Snapshot rejection cases also need a pristine accepted baseline
before each mutation, or an earlier corruption can mask a missing check.

An MT test must keep a peer alive while recording and executing the intended
shared path. Joining the peer first changes the code path being tested. A
registered Lua case now reuses the dedicated numeric-read program instead of
maintaining an incompatible duplicate. Native helper counts must use the
trace lease (`jit_base`); `in_native` changes at the C-call boundary.

The production delta relative to the integrated LuaJITMT comparison point
`489bb089` already touched 74 `src/` files, with 17,140 added and 1,966 removed
lines before the numeric-read change. `lj_trace.c` accounts for 4,592 added
lines and the new admission header for 4,200. Counts include test-only sections;
they measure review surface, not execution cost. New abstractions should
reduce repeated mechanisms and future changes, rather than merely relocate
the same large policy blocks into more files.

## Implementation sequence

1. Completed: bounded no-wait numeric reads with independent admission,
   native lifetime, refusal, type-exit, and both target variants checked.
2. Completed: redundant entry scan removed as a separate measured change;
   all lifetime and mutable-state revalidation retained.
3. Add workload reporting for trace rejection and exit reasons, then prioritize
   common numeric transitions and scalar side traces.
4. Generalize rooted helper contracts by key/result/effect class, followed by
   rooted stores and allocation. Keep raw heap operations closed until their
   own lifetime and publication mechanisms exist.
5. Consolidate test build profiles and fixtures, then expand sanitizer,
   weak-ordering, stalled-participant, and sustained-concurrency coverage.
6. Measure release performance and merge focused generic fixes upstream in
   small commits. Do not infer full port parity from stock-suite passes.

The recorded `m6_jit_aref_pair_boundary` case still tests broad variable-key
reads and hash paths outside the admitted ARM64 capability. Its old TValue
helper expectation is not a reason to admit those graphs or silently waive
the assertion. The new numeric-read case covers its own exact contract;
broader M6 parity needs its own implementation and target evidence.

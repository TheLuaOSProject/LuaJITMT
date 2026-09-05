# String retention and automatic GC baseline, 2026-09-05

Frozen runtime: `e34282576c7df0180e8113a4cfba07fd637a36b3`, Linux x86-64.
Package: `/tmp/lj-string-retention-baseline-20260905-424nvcfy`.
No runtime change, shared-workspace edit, or commit was made.

The completed-cycle control confirms the documented string retention defect.
An ordinary persistent Lua peer and a configured GC worker pool independently
prevent the current explicit sole-main string-body reclamation path. Automatic
collection has a separate, history-dependent admission/progress problem. These
are separate findings; a failed automatic cycle cannot prove string retention
despite completed collection.

## Final fixture and measurements

`t-string-retention.c` plus `peer-control.lua` use public Lua APIs and ordinary
`threading.spawn`, channels, `gcworkers`, native sleep and join. There are no GC
test helpers, forced phases, hook replacements, internal collector calls, or
admission changes. The C diagnostics read runtime counters without flushing
them or modifying runtime state.

The fixture maintains 32 registry-rooted, 64-byte strings. Each round creates
and drops 4,096 different 64-byte strings with `lua_pushlstring`/`lua_pop`. Six
rounds therefore create 24,576 unreachable bodies. The peer remains alive for
the entire measured interval and checks the same anchors after each round;
it waits on a channel between requests. A configured pool remains at two
workers until the separate cleanup phase. The automatic driver overwrites a
32-slot ring with ordinary Lua tables, so its semantic live table set is fixed.
It makes at most 64 bursts of 4,096 allocations, with eight 2 ms native waits
per burst to give GC workers scheduling opportunities. It does not call
`collectgarbage`, `lua_gc` collection/step, or any internal collector entry.

Counts subtract the main TG's unused string-count credits and the peer's
locally published credit. The peer creates no new strings after publishing
that credit: all of its string interns are hits on rooted anchors, and its
channel traffic is numeric. GC worker TGs have no Lua stack and never intern
strings. All final matrix measurements have zero unused credits, so their raw
and adjusted counts agree exactly. All begin with 300 interned strings.

The rooted pointer is compared against a new ordinary intern of identical
bytes, and its contents and Lua equality are checked on both Lua owners.
Only rooted strings have saved C pointers. No garbage-body address is retained
or dereferenced by the fixture.

`gc2_cycle_starts` records real MARK admission; `gc2_sweep_to_idle` records the
SWEEP-to-IDLE transition. The latter, not request counts, is the completion
check. Explicit measurements require at least two public full-collection calls
and two completed transitions per round. Automatic measurements require three
more completed transitions; a concurrently started next phase is reported,
rather than incorrectly requiring a sampled IDLE interval.

`sweep_unlinked` and `sweep_reclaimed` are the actual intern-table retirement
and physical body-destructor counters. They are not RSS estimates. On this
build `lj_str_size(64)` is 100 bytes; the 24,576 bodies carry exactly 2,457,600
bytes of allocator accounting. Arena padding and OS mappings are separate.
`bytes` is `lj_gc_total_load`, not RSS or a claim about physical page release.

## Final matrix

Normal, assertion/API-check, and Clang ASan builds agree on the string results.
The table below reports each build's measured interval, excluding setup and
the separate sole-main cleanup.

| Collection | Persistent peer | GC workers | Completed cycles | Extra strings retained | Bodies reclaimed |
|---|---:|---:|---:|---:|---:|
| explicit | 0 | 0 | 12 | 0 | 24,576 |
| explicit | 0 | 2 | 12 | 24,576 | 0 |
| explicit | 1 | 0 | 12 | 24,576 | 0 |
| explicit | 1 | 2 | 12 | 24,576 | 0 |
| automatic | 0 | 0 | 18 | 24,576 | 0 |
| automatic | 0 | 2 | 0 / 1 / 0 | 4,096 | 0 |
| automatic | 1 | 0 | 0 | 4,096 | 0 |
| automatic | 1 | 2 | 0 | 4,096 | 0 |

The worker-only automatic row is ordered normal/assertion/ASan. It started
1 / 2 / 1 real cycles and was in SWEEP after 262,144 filler allocations. Its
workers made real asynchronous progress; this bounded result is not a proof
of permanent deadlock. It did not complete the required three transitions.
The two persistent-peer automatic rows never admitted the pending IDLE request
in any build. All nine incomplete matrix processes exit 2 after normal cleanup;
they are deliberately not counted as passing completed-cycle cases.

The sole-main explicit byte count reaches a stable plateau after the first
batch: 222,246 bytes in the normal build, 222,278 in assertion/ASan. The 61,440
bytes above startup correspond to the larger intern-table header. With a peer
and workers, the final explicit snapshot has 24,876 strings and 2,908,158 /
2,908,190 / 2,908,190 accounted bytes. The common increase is 2,715,648 bytes:
2,457,600 body bytes plus 258,048 bytes of intern-table growth. After stopping
the workers, joining the peer, and performing fresh full collections, every
final matrix case returns to exactly 300 strings, with equal cumulative
unlinked/reclaimed counters and empty current/sealed retirement batches.

The enlarged string-table mask remains 32,767 after recovery from the retained
six-batch history. Body reclamation therefore does not demonstrate table
capacity shrink or a return to the original complete heap footprint.

## Last-child restoration control

`t-string-retention-recovery.c` differs only by observations and an additional
automatic-only allocation interval after cleanup of the peer/pool but before
any explicit collection. `recovery-fixture.patch` records that exact fixture
delta; it is not a runtime patch. Six fresh normal/assertion/ASan processes
cover initial worker counts zero and two.

While the peer lives, the interpreted automatic request remains in IDLE:
`cycle_requests` advances 4 to 5, `cycle_leader` is 1, but starts/completions
remain 4/4 after 262,144 table allocations. `alloc_since_trigger` reaches
27,777,800 bytes, far beyond the roughly 770 KB hard threshold. Disabling the
pool leaves `g->gc.threshold` at `LJ_MAX_MEM` (140,737,488,355,328). Joining the
last peer restores that field to the exact finite `gc.mt_threshold` value:
586,850 normal or 586,882 assertion/ASan in these processes.

Without an explicit collect or step, the next 8,192 ordinary Lua table
allocations complete three cycles: starts/completions 4/4 to 7/7. The accounted
heap falls from about 27.99 MB to 0.729 MB, but all 4,096 garbage strings are
still retained and the string-destructor counters do not advance. The later
explicit sole-main control finally reclaims them. This separates automatic
collector admission, general heap reclamation, and string-body reclamation.

## Counterexample to a universal MT automatic-GC ban

`trace-trigger-late.lua` performs only 8,192 ordinary table allocations followed
by first compilation/execution of a simple numeric loop. It does not issue
collect or step. Compilation settings are changed only inside the measured
workload, after the baseline. Six fresh normal/assertion/ASan processes cover
one continuously live peer and either zero or two configured workers.

With JIT enabled, all six observe a new actual MARK cycle and reach SWEEP:
starts/completions 4/4 to 5/4, while `mt_live == 1` and the configured pool is
still unchanged. With the exact same assertion binary and Lua source but the
JIT engine disabled, both controls remain 4/4 in IDLE. The short Lua variant
deliberately returns after that observation, so its exit 2 does not represent
an attempted three-cycle bound. It demonstrates admission, not full-cycle
completion or native allocating-trace coverage. `jit_hard_checks` is zero.

The source explanation is the trace-completion call to `lj_gc_step` described
below. An earlier, separately retained long numeric-trigger experiment with
one peer and no GC workers completed 18 automatic cycles and retained all
24,576 strings. Its early global hotloop setting is not part of the final
controlled experiment; the corresponding two-worker setup timed out before
printing its baseline and is not measured-workload evidence.

## Source review and smallest next scope

Source references below refer to the frozen commit, available in `normal/src`.

1. `lj_api.c:3009` alone creates the explicit string `reclaim_requested` request
   around the protected full collector and cancels it afterwards.
   `lj_str.c:1254` requires the internal allocator, actual main TG, no active
   trace, no quarantine ownership, and no pending finalizer work. After its
   exclusion CAS/fence, `:1282` rejects live/entering secondary mutators,
   configured workers, or an existing batch. `:1748` is the actual sweep
   admission. `LJ_GC2_STRING_BODY_RECLAIM` is defined as zero in `lj_str.h`, but
   has no implementation reference in the current source: it is not a switch
   whose removal would complete the protocol.
2. `lib_threading.c:886` saves the old threshold at the first child and puts
   `LJ_MAX_MEM` in the legacy/global threshold. `:924` restores the saved
   logical MT threshold only when `mt_live` decrements from one to zero.
   `lj_api.c:2972`, `lj_gc.c:66`, and `lj_gc2.c:2837` maintain or consult that
   separate logical running/stopped value. A public restart does not by itself
   replace the child-lifetime global-threshold gate.
3. Allocation accounting already publishes a durable IDLE request:
   `lj_gc2.c:2860`, `:3227`, `:3249`. Its request threshold updates the MT
   threshold while a child is live. The configured worker loop cannot consume
   that IDLE request: `:22369` refuses phases outside MARK/WEAK/SWEEP.
   `:14975` hard assist likewise performs active MARK/WEAK work, not MARK
   initialization. `lj_gc.c:4281` invokes `gc2_step_auto` only when the sampled
   global threshold is due. The generated interpreter predicate at
   `vm_x64.dasc:419` additionally waits for local allocation debt before its
   hard-only path; slow allocation checkpoints can flush that local debt
   before the next interpreter test. The pending request can therefore remain
   published but unconsumed.
4. This behavior has a real alternate entrance. After successful trace
   completion, `lj_trace.c:6352` invokes `lj_gc_step` when GC pressure is due.
   `lj_gc.c:4146` uses the logical running predicate and can drive an existing
   IDLE request even when its `threshold_step` input is false. Its active-phase
   tail at `:4200` publishes a finite global threshold, which can restore later
   interpreted driving while the child still exists. This is the source-based
   explanation of the controlled numeric-loop result; the claim is not that
   every JIT-enabled allocating loop reaches this path.

The smallest useful implementation scope is **reliable consumption of an
already published automatic request at an ordinary safe VM boundary**, with
stop/restart semantics and first/last-child transitions preserved. Reconcile
the C and generated interpreter scheduling predicates with the logical MT
running state; merely making `gc_step_assist_top` enter the driver on a
hard-only call may not help when allocation checkpoints already flushed the
local-debt predicate. Do not invoke a full collector from an arbitrary raw
allocation/accounting point where object construction or stack geometry is
unfinished. Cover pool zero/two, interpreter/JIT, first attach, last detach,
explicit stop/restart, a prepublished request, native return, and a paused
participant using the existing root/action hold rules. Worker-only SWEEP
completion needs its own scheduling/ownership analysis; this baseline does
not identify that row's final blocking owner or justify removing its token.

Keep string retirement as a separate implementation transaction. Existing
sole-main batches retain exclusion through irreversible unlink, both graces,
and the exact IDLE reclaimer (`lj_str.c:1353`, `:1363`, `:1893`;
`lj_gc2.c:6518`). An automatic driver may return mid-cycle, so simply admitting
those batches from automatic GC could strand exclusive attachment admission
behind a suspended original driver. Existing quarantine setup/lookup is not
the full publication-vs-commit and native-byte-borrow protocol. Before enabling
concurrent body free, generic late publication must atomically arbitrate with
logical canonical death, native/FFI/raw-byte borrowers must retain lifetime,
and a durable canonical owner must survive unlink, metadata failure, address
reuse and shutdown. Do not delete the peer/worker gate as a memory fix.

## Validation and reproduction

`summary.json` contains the final 24 matrix processes and 14 additional
processes. The 24 comprise 15 completed measurement cases and 9 incomplete
automatic progress cases. The 14 comprise 6 successful last-detach recovery
controls and 8 short JIT admission/control observations. All final processes
perform normal cleanup, and all final stderr files are empty. ASan uses
`detect_leaks=1:abort_on_error=1` with no suppressions. `nm` confirms ASan
symbols in the target `lj_str.o`, none in host `minilua` or `buildvm`.

`source-identity.json` pins all 224 source/generator inputs and the source
archive. `final-source-verification.json` verifies every input in all three
trees and rechecks built artifacts. `*-build.json`, `*-inputs.json`, and
`*-results.json` retain exact argv, environment settings, source/archive/ELF
hashes, duration, status and output filenames. `build.py`, `run.py`, and
`extra.py` reproduce the runs. The source trees were extracted using
`git archive e34282576c7df0180e8113a4cfba07fd637a36b3 src dynasm Makefile .relver`.
Run `python3 build.py normal`, `python3 run.py normal`, and corresponding
`strict`/`asan` commands from a package with those isolated directories.
`run.py` returns 1 overall when the matrix contains its deliberately retained
exit-2 progress failures. It does not silently reinterpret them as successes.

Development controls remain in the package with their original inputs:
the first C-only automatic driver did not prove VM automatic progress;
the first Lua worker-only observations missed the completion bound; early
hotloop configuration timed out before the measured baseline; and the first
short JIT recovery required another fresh full-collection API call after
entering cleanup in SWEEP. The final `full_cycles` requires two API calls as
well as the completion counter, rather than accepting one call which merely
finishes an old cycle plus a fresh one. An initial summary assumption that the
worker-only automatic row always completes zero cycles is retained in
`summarize-first-assumption.py`; the final assertion run actually completed
one, so the report preserves the observed 0/1/0 counts.

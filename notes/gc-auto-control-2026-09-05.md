# Automatic GC control and request admission, 2026-09-05

Public STOP/RESTART now has an atomic control state independent of derived
pacing thresholds. A delayed first-attachment or last-detachment threshold
store can no longer reverse a completed public control operation. Existing
safe interpreter/C boundaries also consume a durable IDLE request that the MT
threshold bridge previously hid. Temporary finalizer suppression remains an
independent admission veto. The collector's synchronous driver, worker-two
SWEEP completion and concurrent string reclamation remain open work.

The integrated six-file source is the frozen candidate3 full patch
`5f8a22b6bc1ae8956f63844cc0ad8bd63b093db98070078eeb391ee5e48ed9a1`
against `0e2119e4`. All 224 ROOT runtime/generator inputs match the tested
normal, assertion/helper and target-only ASan source copies. The final shared
default executable is
`a399c29f009c8deecc04de97cb0310205be2c9d1cc305e0a66c70eeb41757013`;
the archive is
`abd9ede84ae1399adfd27c11a5079f69d5060a6fd45e7cc9f80cf01b7438eb97`.

## Control, callback scope and compatibility

GCState's unused byte becomes `auto_flags`, without moving adjacent fields.
Initialization clears it once. STOP atomically sets STOPPED before pacing
stores; RESTART publishes pacing and then clears only STOPPED. The API operation
selects the update explicitly. A legitimate restart pause calculation can
numerically equal MAX, so threshold equality cannot select logical STOP.
Attachment, detachment, callback threshold restoration and cycle pacing never
write the control byte.

Actual callbacks set FINPAUSE after successful state claim and stack
preparation, beneath the existing physical finalizer owner. Same-owner nesting
retains the outer pause; foreign actors cannot dispatch another callback under
that owner. Protected callback errors return through restoration of current
state, hooks/profile and pacing. Only the scope that first set FINPAUSE clears
it, before finalizer-error delivery. Callback/peer STOP survives that cleanup;
RESTART and the existing nonthrowing full-collect API return clear STOPPED
without clearing FINPAUSE. Full collection during an active finalizer still
defers under its existing contract.

Public ISRUNNING tests STOPPED only. Internal automatic admission tests both
STOPPED and FINPAUSE. Thus a callback, an existing peer and a newly spawned child
observe the same public setting, while automatic admission remains paused
through the callback. This deliberately changes the earlier sole-main callback
query too: without a public STOP it reports true during the temporary pause.
It does not report the current phase or whether a worker is executing.

Candidate2 instead exposed FINPAUSE through ISRUNNING. That failed the unchanged
finalizer-spawn-live test in three of six candidate runs; other schedules sampled
after callback cleanup. A separate production diagnostic makes the overlap
deterministic by having the actual callback wait for the child's first query.
The old control reports running during and after; candidate2 reports stopped
during and running after. Candidate3 preserves the original test and supplies
new explicitly versioned oracles for the selected public setting. The
[candidate2 handoff](evidence/gc-auto-control-2026-09-05/candidate2/package/HANDOFF.md)
retains all 247 outcomes and the preceding source review. The
[superseding review](evidence/gc-auto-control-2026-09-05/candidate3/independent-review/review.md)
records why its public-query decision changed after validation. This is an
intentional fork contract, not an exact upstream callback-behavior claim.

The C and x64 pending-request checks require a durable nonzero request, IDLE
and no automatic-control veto. Existing GC-safe targets still prepare the Lua
stack and enter the same driver. Raw allocation accounting only publishes a
request; it does not enter a collector with incomplete stack geometry. The
driver independently samples control. These are invocation-entry guarantees:
STOP does not cancel already-authorized work, and a nonzero request is not a
new owner certificate. Explicit collection, workers and all root/native/phase
and physical reclamation gates retain their prior authority.

## Validation

There are **608 passing functional runtime processes** for the final source:

| Group | Passing processes | Evidence and limits |
| --- | ---: | --- |
| Candidate3 control/finalizer/matrix | 124 | Another 27 worker-two processes retain their SWEEP completion failures |
| ROOT stock/JIT/FFI, three builds | 294 | 98 per normal, assertion/API-check/helper and target-only ASan build |
| Canonical `m3_gc2_auto_control` | 37 | All permanent GCC cases pass on the final shared default build |
| Canonical `m7_ffi_clib_cache_authority` | 153 | Existing root/side/cache/lifecycle/recorder checks pass on the same build |

The ROOT group includes stock JIT-off 387 and JIT-on 509 tests in each build,
mutable namespace methods, captured receivers, pure userdata/cdata guard
exclusions, original cache lifetime, extern snapshots, metadata refusal,
callback stack restoration and real generated remote FFI flush. Stock test
counts are assertions/subtests within six subprocesses, not added again to
the runtime total. Exact commands, flags and results are in the
[ROOT validation](evidence/gc-auto-control-2026-09-05/root/final-validation.json).

The [candidate3 handoff](evidence/gc-auto-control-2026-09-05/candidate3/package/HANDOFF.md)
records 151 processes: 124 pass and 27 miss the existing worker-two SWEEP bound.
Its 49 ASan processes contain no sanitizer report, including bounded failures.
Throwing finalizer controls intentionally produce error messages; stderr is
not universally empty. The unchanged helper assertions and construction/IDLE
waits in preceding packages are neither rerun nor repaired by the query change.

Real first-live STOP overlap preserves the request without beginning MARK.
Exact GCC/x64 trap schedules pause actual first-attachment and last-detachment
MOV instructions after capturing MAX; public RESTART completes before the
delayed real stores resume. The new runtime keeps logical running and completes
two cycles within 8,192 TNEW allocations. The previous veto controls lose
RESTART. The separate numeric-MAX fixture creates its threshold through an
ordinary anchored userdata and public SETPAUSE/RESTART; it also completes two
cycles. No control count, threshold, phase or request is fabricated.

The permanent gate preserves ten new source/input files byte-for-byte and
uses the existing M8 spawn-live test unchanged. It runs all 16 peer/nested-error/
outer-error/outer-STOP callback combinations, two Lua query fixtures in both
JIT modes, six ordinary allocation boundaries and seven sequential/native/
active-STOP cases. The two exact delayed-MOV schedules require the verified
GCC runtime geometry; Clang retains the other 35 cases. The control entrypoint
includes the string fixture source and receives its two Lua input paths as
arguments; no fixture contains an absolute package path. Every canonical
process must exit zero. Known worker-two failures remain separate evidence.

With zero GC workers, automatic churn now completes 18 cycles with or without
a persistent child. The same 24,576 dropped string bodies remain retained
after those cycles; sole-main explicit cleanup reclaims them. Two-worker cases
admit the request but miss later completion in SWEEP. Admission and physical
reclamation are separate requirements, and neither outcome is hidden by a
passing control test.

## Cost and remaining work

Seven alternating matched pairs per workload, pinned to CPU 30 on a shared
host, show no measured cost increase in these selected cases. Each process
uses the minimum of five CPU-clock passes with GC enabled. Six calibration
processes are preserved separately. Table allocations escape through a fixed
live ring; JIT-enabled labels describe configuration, not a claim that every
iteration stays native.

| Workload | Previous median ns/iteration | New median | Change |
| --- | ---: | ---: | ---: |
| Escaping TNEW, interpreter | 7,936.13 | 7,852.59 | -1.05% |
| Escaping TDUP, interpreter | 7,109.45 | 7,105.40 | -0.06% |
| Escaping TNEW, JIT enabled | 7,572.57 | 7,538.25 | -0.45% |
| Arithmetic, JIT enabled | 0.684 | 0.684 | 0.00% |
| FFI struct, JIT enabled | 0.6867 | 0.6867 | 0.00% |

These small differences do not establish a speedup. Arithmetic/FFI values use
the existing harness's rounded total time and exact iteration count. All 70
measurement processes pass; [raw samples and summary](bench/gc-auto-control-2026-09-05/cost-summary.json)
are matching-fork checks, not stock parity or MT scaling evidence.

The [complete archive](evidence/gc-auto-control-2026-09-05/artifact-manifest.json)
contains 1,311 verified text/source artifacts and 164 hash-only binary/archive
identities. It preserves candidate2's rejected public-query behavior,
candidate3's exact sources/results/review, ROOT combined evidence and compiler
artifacts. Raw whitespace is unchanged. The earlier unsafe admission and
STOP-veto experiments remain in the
[preceding review](gc-auto-control-review-2026-09-05.md).

Next resolve worker SWEEP progress and unfinished-owner deferral, then replace
synchronous phase/root ownership and concurrent string lifetime exclusions.
This repair is Linux-validated and is not release readiness or completion of
the lockless goal.

# Automatic GC admission and control-state review, 2026-09-05

The automatic-GC experiments expose a control-state race that must be resolved
before the new admission path is integrated. A completed public STOP can be
misread during first attachment, and a completed RESTART can be undone by a
delayed attachment threshold store. The committed GC runtime is unchanged by
this review. The experiments use frozen `597b8705` inputs; the eight inspected
GC, API, threading and string source files still match integrated `aee88db5`.
This is not combined-runtime validation of the prototype against that HEAD.

The [initial three-file candidate](evidence/gc-auto-control-review-2026-09-05/initial-admission/HANDOFF.md)
consumes durable IDLE requests at existing safe VM/C boundaries. It restores
admission with a live peer when the lifetime threshold otherwise hides the
pending request. Its patch hash is
`f632c7f26051ffebb55fde65291a304b91a46df0371cf47f86aa1783efdf5cd9`.
It is unsafe to integrate alone: the new path starts MARK after a completed
STOP in the controlled first-child overlap below.

A real external attachment publishes its first live count before copying the
global threshold into the saved MT threshold. A previous attachment has left
a finite saved threshold. With main-thread STOP already complete, the global
threshold is MAX, but that short first-live window exposes the stale finite
saved value. Linux trap-flag observation pauses at the actual attachment
instruction; no phase, request, threshold or live count is fabricated. One
ordinary interpreted TNEW then distinguishes the old entrance from the new
automatic request entrance.

| Frozen runtime | ISRUNNING in the stopped overlap | Starts/completions after TNEW |
| --- | --- | --- |
| Unchanged control | Incorrectly true | 4/4, IDLE |
| Initial admission candidate | Incorrectly true | 5/4, MARK; expected-negative exit 42 |
| Additional STOP veto, normal/assert/ASan | False | 4/4, IDLE; pending request preserved |

The [six-file veto prototype](evidence/gc-auto-control-review-2026-09-05/stop-overlap/package/HANDOFF.md)
adds an independent atomic STOP byte in an unused GCState position. Public
STOP publishes it before threshold changes. Successful public restart clears
it after publishing thresholds. Automatic request creation, the C driver,
the new x64 pending-request entrance and ISRUNNING consult it; attachment and
temporary finalizer threshold stores cannot clear it. The exact full patch is
`35e8abddecefe17ca842d96d4f2f237fd986303ca3eac8b45c1f60d2610f542b`.
The [independent source review](evidence/gc-auto-control-review-2026-09-05/veto-review/review.md)
finds no source blocker for this bounded completed-before-call STOP guarantee.
It is neither cancellation of already-authorized work nor a complete public
control-state publication design.

A separate exact production overlap reproduces lost RESTART on the unchanged
control, initial candidate and STOP veto. The attacher has loaded global MAX
and is paused immediately before the verified MOV into the saved MT threshold.
Public RESTART completes, setting a finite MT threshold and reporting running.
The delayed MOV then restores stale MAX. ISRUNNING becomes false; 8,192 real
table allocations leave starts/completions at 4/4 in IDLE even with allocation
debt above hard. All three return the expected-negative exit 43 and perform
normal detach/close cleanup. Exact instruction identities, disassemblies,
commands and counters are retained. A retry or first-live recheck alone does
not supply durable logical authority for these delayed threshold stores.

When restart instead occurs after attachment finishes, all three veto builds
admit on the first TNEW and complete three automatic cycles with a live child
within 16,384 allocations. The smaller 8,192 bound is preserved separately:
it completed two cycles, then had no pending request and debt below the next
trigger. Increasing that calibration's allocation supply did not change a
STOP check or count explicit cleanup as automatic progress.

The production matrix and six allocation-boundary controls exercise TNEW,
TDUP, string.char, string.rep, public number-to-string and FNEW with real pending
requests, plus STOP/restart, first/last child, active-cycle stop and actual
native return. Normal, assertion and target-instrumented ASan builds preserve
all root, phase, worker and string-reclamation gates. With no configured GC
workers, automatic churn now completes 18 cycles both with and without a live
peer. Configured two-worker cases admit but miss the later completed-cycle
bound in SWEEP. Unreachable strings remain retained under the original policy;
explicit sole-main cleanup restores the baseline string count.

The veto package records 164 fixture processes, including expected negatives,
incomplete progress and safety failures. It is not an all-passing gate. Of its
32 initial existing-safety processes, 27 pass, three assert and two time out.
Matched control evidence reproduces the interpreted hard-assist assertion,
remembered-filter count assertion, exclusive IDLE iteration wait and the
combined-helper closure-construction timeout. Actual debugger stacks preserve
the latter two waits; interrupted debugger exits are not counted as passes.
One worker-scheduler SSB-empty assertion did not reproduce in the package's
subsequent matched runs and remained unresolved at that freeze. A separate
diagnostic investigation is required before classifying it. The 37 ASan
production processes have empty stderr, including their deliberately retained
progress failures; absence of sanitizer reports does not turn them into passes.

The [complete archive](evidence/gc-auto-control-review-2026-09-05/artifact-manifest.json)
preserves 1,063 text/source artifacts and 110 hash-only binary/archive
identities. It includes both prototype generations, both independent source
reviews, exact commands/flags, source identities, raw failures and bounds.
The [ROOT verification](evidence/gc-auto-control-review-2026-09-05/root/verification.json)
records that no GC production patch is integrated. Existing owner packages
remain immutable; subsequent results must be separate evidence.

Next, make public running/stopped control independent of derived attachment
thresholds while preserving the entire temporary finalizer suppression scope,
including nesting, callback STOP/restart and unwind. Reuse the exact formerly
failing overlap schedules. Resolve the scheduler observation and retain the
separate two-worker SWEEP completion failure. The automatic driver is still
synchronous and may handshake or wait for exclusive ownership. Neither this
admission experiment nor its STOP veto completes asynchronous GC or the
lockless goal. Linux remains the current validation target.

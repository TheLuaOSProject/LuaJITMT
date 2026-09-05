The six-file prototype restores admission of a published automatic IDLE GC
request at existing Linux x64 GC-safe boundaries and preserves explicit STOP
across first-child attachment. The original three-file candidate has a proven
STOP regression and must not be integrated alone. This package is a reviewable
prototype, not a release recommendation: the scheduler observation and other
remaining limitations below still need to be carried forward explicitly.

No shared source, shared build, or commit was changed by this agent. All new
work is under this directory. The committed source base is
`597b8705208957ade8465416da30976ab9b52195`; the last read-only observation of
shared HEAD was `bc7fcf457d39dcf065dbea084eb9ff51c32ddff8`. The parent must apply
and validate the combined patch against the actual integration HEAD. These
executables are evidence for the frozen base plus this exact patch only.

The complete patch is `veto-full.patch`, SHA256
`35e8abddecefe17ca842d96d4f2f237fd986303ca3eac8b45c1f60d2610f542b`.
The additional STOP repair relative to the failed candidate is
`veto-only.patch`, SHA256
`4808e3c7b435db176df4e5c4c33ce3bf8729cd3bfa6d5d2f1efd0a7f7c967048`.
Authoritative source files are under `veto/src/`; all six hashes are recorded
in `veto-build.json` and `final-source-verification.json`.

The initial candidate and all of its evidence remain at
`/tmp/lj-gc-auto-admission-20260905-h7ntx71p`. Its `candidate.patch` hash is
`f632c7f26051ffebb55fde65291a304b91a46df0371cf47f86aa1783efdf5cd9`.
The completed original string-retention baseline remains untouched at
`/tmp/lj-string-retention-baseline-20260905-424nvcfy`. Both previous handoffs,
including unsuccessful fixtures and actual debugger stacks, remain relevant.

The first-child STOP race is a real production overlap:

- A prior external child attachment leaves a finite saved MT threshold. After
  it detaches, ordinary public string allocations publish an actual IDLE
  request and flush local debt to zero. Public STOP then completes while the
  main thread is the only live mutator; the global threshold is MAX and the
  previous MT threshold is still finite.
- A real external `lj_threading_attach` publishes `mt_live = 1` before copying
  the global threshold into the MT threshold. Its `mt_entering` reservation is
  still held. No existing ordinary MARK admission gate excludes that window.
- The GNU link wrapper only arms Linux trap-flag stepping at the real
  `lj_vm_cpcall` attachment boundary. The signal handler stops the foreign
  owner after the actual live-count instruction and before the threshold
  copy. No request, phase, live count, threshold, or native certificate is
  fabricated. The observer releases the foreign owner after a real cycle
  start or after the main owner's single real interpreted TNEW returns.
- `t-stop-first-attach.c` is unchanged between all five comparisons. Its
  SHA256 is `3ae6ce71c2cfa62f09bd3752f3736266a8606983a72928d2ef5476d1c16ab640`.
  Each subprocess has a 20-second alarm and 25-second outer bound. All five
  finish normal production detach/close cleanup.

At the controlled window, starts/completions are 4/4, phase IDLE, leader 1,
live 1, entering 1, global MAX, saved MT 291600, local debt 0. The unchanged
control reports ISRUNNING true in that window but does not admit the request.
The initial candidate additionally starts MARK after the completed STOP:
starts/completions become 5/4 and it returns the expected-negative code 42.
The veto candidate under normal, assertion, and AddressSanitizer builds keeps
ISRUNNING false, starts/completions 4/4, IDLE, and the same pending leader 1;
all three return 0. Exact commands, environment, source/archive/ELF hashes,
and output are in `overlap-*`, `veto-overlap-*`, and
`instrumented-stop-overlap-*` files.

The STOP repair uses one independent atomic veto byte. It renames the unused
byte in GCState, so the layout and following offsets do not grow or move.
`lj_gc2_init` initializes it once. Public STOP release-publishes 1 before any
threshold stores. Public RESTART, and the existing successful full-collection
restart path, publish their thresholds and then clear the veto. Attach,
detach, temporary finalizer thresholds, and ordinary pacing never clear it.
The allocation-trigger logical-stop check, the C pending-request predicate,
the actual automatic driver's independent logical-running snapshot, the
x64 pending-request path, and ISRUNNING all consult the veto. The old logical
threshold restrictions remain. Existing hard assists and active worker work
remain unchanged; explicit STEP/COLLECT retain their existing authority.

This guarantees STOP completed before a subsequent automatic invocation
cannot be defeated by stale first/last-child threshold stores. It is not a
cancellation barrier for an already in-flight automatic invocation, or a new
linearizable arbitration of competing public STOP/RESTART calls and MARK.
The driver still samples its running state once per invocation. No root,
action, native, JIT, activation, worker, phase, or string-reclamation gate was
removed. The broad nonzero request check does not grant the GCSCAN sentinel
ownership: existing worker claim and MARK entry both reject it.

Independent source review of the exact six-file patch is copied in
`independent-review/review.md`. Its immutable original is
`/tmp/lj-gc-auto-stop-veto-proof-20260905-gqkrx2u7`, with review SHA256
`eb90f485d2ba5de9dde4a5d92b3b3e3d50c830a1d3b77e692d78b1587d24170d`.
The reviewer found no source blocker for this bounded STOP scope. The earlier
source review and counterexample package also remain immutable at
`/tmp/lj-gc-auto-admission-proof-20260905-mrosx7pd`.

A separate preexisting RESTART loss is now demonstrated, not just inferred.
`t-restart-first-attach.c` single-steps the real GCC/x64 attachment until the
exact emitted MOV into the saved MT threshold is next. It verifies the MOV's
base, displacement, and source register: the attacher has already captured
global MAX. Public RESTART then completes and publishes a finite MT threshold
(448200 in normal control/veto). When the delayed store resumes, it overwrites
that restarted MT threshold with captured MAX. ISRUNNING becomes false and,
after 8192 real TNEWs, starts/completions remain 4/4 in IDLE even though debt
925540 exceeds the hard limit 583440. Pristine control, initial candidate,
and veto all return the expected-negative code 43 and clean up normally.
The exact binary disassemblies and commands are in `restart-overlap-*`.
The veto intentionally does not make logical RUNNING authoritative or cure
this existing threshold-publication liveness defect.

An additional production control restarts only after the stopped overlap's
attachment has completed, then drives real TNEWs with the child still live.
The unchanged control never admits the pending request. All three veto
builds admit at the first TNEW and complete three actual automatic cycles
(4/4 to 7/7) within 16384 allocations, before explicit cleanup. These are
`t-stop-first-attach-restart-v2.c` and `stop-overlap-restart-v2-*`.
The initial 8192-allocation version is retained separately and returns 44
for all four builds. It did show immediate admission and two completed cycles
in each veto build, then IDLE with no request and debt 230160 below the next
trigger. The 16384 follow-up supplies enough ordinary allocation to request
the third cycle; it does not change a STOP check or count completed cleanup
cycles as automatic progress. Both bounds and all outputs remain visible.

The main production and boundary fixtures are byte-for-byte copies of the
previous package's inputs. Every measurement counts actual start and
SWEEP-to-IDLE completion counters; none treats a full-collect call count or
RSS as proof of reclamation. All primary runs use the interpreter engine.
The existing safety Lua scripts also run with JIT disabled and enabled.

Normal, assertion, and target-only ASan builds each ran the following:

- Eight ordinary string cases: explicit or automatic collection, persistent
  peer 0 or 1, configured workers 0 or 2. Both worker-zero automatic cases
  complete 18 actual cycles over six rounds and 49152 fixed-ring table
  allocations. The 24576 ordinary unreachable strings remain retained,
  exactly as in the original string policy. Two-worker automatic cases
  admit, complete one cycle, then remain in SWEEP after the 262144-table
  bound; actual worker progress increases, but the requested completion
  target is missed. This is a bounded progress failure, not a proof of a
  permanent deadlock.
- All six existing safe admission boundaries with a live peer and workers
  0 or 2: TNEW, TDUP, x64 string.char, C string.rep, public C number-to-string,
  and FNEW. All enter MARK from the already-published request even though
  local accounting debt is exactly zero and global debt is below hard.
- Public STOP/RESTART, stopped last-detach/first-attach, and actual native
  sleep/return controls. STOP preserves the exact pending request and no new
  start/request/completion is attributed to ordinary allocations. A native
  return is correctly observed through the real wrapper, but is not itself
  claimed to be an automatic collector driver.
- Active-cycle STOP controls for peer 0/1 and workers 0/2. Existing active
  worker progress may continue, while no new request or cycle is authorized.
  After public RESTART, worker-zero controls complete their three-cycle
  target. Last live detach restores the exact saved MT threshold globally.

All worker-zero sequential controls pass. All two-worker controls admit the
request but miss their later automatic completion bound in SWEEP (code 2),
as in the initial candidate. No bound or gate was relaxed to pass them. All
normal cleanup paths disable configured workers, join the peer, then use
explicit full collection; anchored canonical string identities and exact
string counts return to their baseline. Explicit sole-main/no-worker cycles
reclaim string bodies; any persistent peer or configured worker still blocks
the existing physical string-body admission gate. The table header capacity
can remain larger, so exact accounted heap bytes need not return to their
original value even when string bodies do.

`summary.json` contains per-case counters and complete result-file references.
There are 118 normal production/overlap runtimes in this package, including
the intentionally retained negative controls and the smaller completion
calibration. There are 37 target-instrumented ASan production runtimes;
all have empty stderr and no sanitizer report, including incomplete worker
progress and the retained 8192-allocation calibration. Assertions/API checks
are enabled in the strict and ASan runtimes. No test helper macro is enabled
in any of those three production variants.

The unedited existing safety suite produced 32 runtime processes: 27 passed,
three aborted, and two hit the 60-second bound. Eighteen fixture compiles all
passed. The original candidate's known hard-check, accounting, and exclusive
IDLE wait failures remain visible:

- `t-gc2-interp-hard-check.c:230`, missing interpreted hard assist, despite
  observing its hard-check counter. It also failed on the matched unchanged
  control in the previous package.
- `t-gc2-alloc-account.c:929`, remembered-filtered exact-count assertion. It
  also failed on the matched unchanged control in the previous package.
- `t-jit-idle-reclaim-entry`, timeout. The previous matched control's actual
  stack remains in `control-idle-reclaim-gdb.*` in the initial candidate
  package: VM IITERN waits in `lj_tab_wait_l` while the fixture has paused
  the exclusive IDLE writer in `gc2_idle_reclaim_enter`. No fixture change
  or counter adjustment was made here.
- `t-func-construction-anchor` now also times out under the combined six
  helper macros. Matching pristine control, initial candidate, and veto all
  time out with identical flags. Separate three-second interrupted debugger
  observations place all three inside the injected full collection from
  `func_finduv_nothrow`, at fixture line 193. This is before the concurrent
  open-upvalue subtest. The older four-helper candidate run had passed;
  this configuration difference remains explicit. The diagnostic debugger
  exit code 0 is not counted as a fixture pass.
- The first veto `t-gc2-worker-scheduler` run aborted at line 1265 waiting
  for the SSB queue to become empty. An independent old-candidate/veto
  rerun and three subsequent matched runs of each pristine control, initial
  candidate, and veto all passed. This is an unresolved intermittent
  observation, not a proven baseline failure or a reason to discard the
  first result. Its original stdout/stderr are retained.

Pacing, JIT hard checks, activation vetoes, MARK-close progress, JIT MARK/SWEEP
cooperation, pending root publication, userdata/string construction gates,
spawn/native unwind, all eleven native-root/remote-completion controls,
active Lua roots, configured-worker Lua tests, and peer finalizer collection
passed. `scheduler-isolation-*`, `*-matched-safety-*`, and
`function-timeout-gdb-*` hold the additional matching commands and results.
Across production and safety there are 164 normal runtime processes here;
the exit counts are deliberately not summarized as an all-green test run.

All five built source trees were checked against all 807 archived inputs.
The four veto variants differ in exactly the six intended files; the fresh
control helper tree differs in none. All built runtime/archive hashes still
match their build records. Target `lj_str.o` contains ASan references in the
ASan build, while host minilua/buildvm contain none. The source archive and
its identity are referenced by `source-identity.json`. `build.py`, `run.py`,
`controls.py`, `controls-v2.py`, `safety.py`, and `matched-safety.py` record
the reproducible runner logic. Copy this package before rerunning; the
default runners write their result filenames in their own directory.

The smallest next work remains separate and proof-driven. Classify the
intermittent scheduler result before calling the full safety gate clean.
Repair logical GC control publication with an explicit model for overlapping
first/last attachment and STOP/RESTART, including temporary finalizer
suppression; the independent STOP veto alone cannot establish restart
liveness. Isolate the two-worker SWEEP completion condition with real
completed-cycle witnesses and retain its owner/root/action prerequisites.
Only after automatic progress is understood should the separate persistent
peer/worker string-reclamation proof expand its canonical interning and
native borrower lifetime protocol. Deleting those gates is not a repair.

The automatic driver called by this admission patch remains synchronous. It
can perform root/action handshakes and exclusive waits. Its collector-unit
budget does not prove a wall-time bound or another thread's ability to
finish the work. This patch adds admission and explicit STOP preservation;
it does not complete the user's asynchronous, nonblocking, lockless goal.

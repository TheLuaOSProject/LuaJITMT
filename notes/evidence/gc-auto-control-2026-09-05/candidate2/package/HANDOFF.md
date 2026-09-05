Candidate2 repairs automatic request admission and the demonstrated STOP and
RESTART control races at Linux x64 VM boundaries. Its authoritative control
state survives delayed first-attachment and last-detachment threshold stores,
and its explicit finalizer pause survives nested callbacks, callback errors,
and public STOP/RESTART. These are bounded control/admission results. The
automatic driver remains synchronous, configured-worker SWEEP completion is
still incomplete, and concurrent string reclamation is unchanged.

Candidate2 also makes an observable ISRUNNING change which fails an existing
test. The unchanged finalizer-spawn-live fixture failed in three of six new
candidate runtimes; a separate deterministic production diagnostic confirms
the exact changed query interval. This must be an explicit API compatibility
decision before integration. Do not represent this package as all green, a
release recommendation, or completion of the lockless goal.

After reviewing the compatibility result, the parent selected a superseding
candidate3 contract: public ISRUNNING reports the explicit STOPPED bit only,
while internal automatic admission continues to honor FINPAUSE. That future
candidate is to be built and validated in a new isolated package. This frozen
candidate2 source, all 247 results, old query oracles and source-only review
remain evidence of the preceding behavior; they are not candidate3 results.

All source edits and builds are isolated under this directory. This agent made
no shared source/build change and no commit. The archive base is
`aee88db569b82216b705408f00295a337a7393fe`. The parent subsequently repaired the
scheduler fixture in `0e2119e4a36a7034f1f6e1115849677a4ce4359e`; a byte-exact
copy of that test is validated here against the frozen candidate2 runtime.
The parent must validate its actual combined integration source separately.

## Frozen inputs and variants

`runtime-source.tar` SHA256 is
`158b4e6c572515bb0f175ea7db54521f036f39e60e882de3196c86cd20939282`.
`source-identity.json` records all 815 archived input files.

The final six-file candidate is `candidate2/src/`. Its full patch against the
archive base is `automatic-control-full.patch`, SHA256
`60b9c725d89b50daf06f89aab8582dffa62f84277f4fe23ec5fb780a18b568a5`.
Its incremental patch over the earlier STOP-only veto is
`automatic-control-v2.patch`, SHA256
`13f32a17375b92dcbc7d44bd2fc22a686505ae591cdd2ba3260f4fd582caf6a2`.
`candidate2-source.json` records all six final source hashes. They match the
normal, assertion/API-check, ASan, and four-helper source copies exactly.

`control/` is the archive base plus the preceding six-file STOP veto. It
retains threshold-derived logical state and still loses RESTART. Its matched
four-helper build is `controlhelpers/`. `candidate/` is the first authoritative
control prototype. It passes the lost-RESTART schedule but still mistakenly
infers STOP when a computed pacing number equals MAX. Its incremental patch
is preserved as `automatic-control.patch`, SHA256
`a08f9b2cee7e974e8cd2128917beaaba80ec9548e53185ea88d246596e5fa9f7`.
Neither of these trees or their failed outputs was overwritten.

The final normal interpreter SHA256 is
`1575493dc4e71fde6ebfbb962b8d7ac529330af1208ce25ea7cebd63f059c40b`;
the normal archive is
`c4d299474e7f2e3707ac46ac9641e1741d105d33d480d8a28a15a8e80aee8ed6`.
Every variant's exact make argv, duration, executable/archive sizes and hashes
are in `*-build.json`. Production variants have no test-helper macros. Strict
uses assertions and API checks. ASan uses clang, target-only AddressSanitizer,
`-O1 -g`, assertions/API checks and leak detection. The helper build uses
exactly GC2, trace, table and function test-helper macros, plus assertions/API
checks. All 815 inputs are compared in `source-validation.json`.

## Control protocol and its limits

GCState's previously unused byte becomes `auto_flags`, without growing or
moving adjacent fields. STOPPED is bit 1 and FINPAUSE is bit 2. Initialization
clears the whole byte once before publication. Thereafter public STOP and
RESTART use atomic byte fetch-OR/fetch-AND on STOPPED alone; callback scopes
operate on FINPAUSE alone. Readers acquire-load the byte. No attachment,
detachment, finalizer-threshold, cycle-reset or pacing path writes it.

Public STOP publishes STOPPED before its derived pacing stores. RESTART
publishes pacing first, then clears STOPPED. The explicit API operation
selects these actions: `api_gc_setlogical` receives `stopped`, rather than
deriving a logical decision from a number which can legitimately equal MAX.
LUA_GCSTOP is the only caller selecting STOP. LUA_GCRESTART and the existing
nonthrowing full-collect return path select RESTART. A throwing full collect
returns before that update. Pause/stepmul changes do not change the flags.

First attachment still publishes live before saving the old global threshold;
last detachment still restores a saved MT threshold after decrementing live.
Finalizer cleanup still restores saved pacing. These stores can be delayed or
overwrite newer pacing, but cannot overwrite logical control. Overlapping API
calls order their public control changes by their atomic bit updates; a later
derived threshold store cannot reverse that order. Threshold CAS/rechecks,
live-count double sampling, and mt_entering checks alone cannot repair delayed
stores or a later threshold ABA. The helper soft limit is also unsuitable as
authority because normal cycle initialization resets it to MAX.

`lj_gc_pending_auto_request` requires a nonzero durable request, IDLE phase,
and zero control flags. The C boundary predicate and x64 generated predicate
both retain this request even after allocation accounting flushes local debt
to zero below the hard-assist cadence. They branch to existing GC-safe VM
targets with the existing Lua stack/root setup. The actual automatic driver
independently samples the flags; raw allocation request publication checks
the same state. Existing threshold/hard-assist entrances and active-cycle
authority are retained. Nonzero leader is not a new ownership certificate:
the existing worker claim and MARK entry reject the GCSCAN sentinel.

STOP/PAUSE is an invocation-entry guarantee. A flag update does not cancel an
already-authorized automatic invocation and is not atomic with later MARK
publication. Explicit STEP/COLLECT and active workers retain their previous
authority. The driver can wait for existing roots, phase ownership and peers.
Native threshold/hard-cadence geometry is unchanged; the observed real native
entry/return controls do not prove automatic completion for every unbroken
native allocation loop. No root, action, native, JIT, activation, phase, worker
or physical string-reclamation gate was weakened.

## Finalizer ownership and restoration proof

All production `gc2_call_finalizer` callsites are reached by actual userdata
or cdata dispatch beneath `lj_gc2_finalizer_dispatch_one`. That function
acquires the existing physical finalizer owner before dispatch and retains it
until callback dispatch/dequeue cleanup returns. The owner admits same-owner
recursive callbacks with an active count and excludes foreign callbacks.

The new PAUSE interval starts only after callback state claim and fallible
stack preparation have succeeded, and before the old threshold pause. An
ownership assertion precedes the first flag update. Subsequent threshold and
hook operations are nonthrowing; trace_abort requests abort, and profile
dispatch update can wait but does not throw through this scope. The protected
callback returns through `lj_vm_pcall_unwind` on success or callback error.
Current L, hook/profile state and thresholds are restored before the scope
clears its owned PAUSE bit. This happens before potentially throwing ERRFIN
delivery and error cleanup. Failed preconditions return before setting PAUSE.

The physical owner covers the complete interval, including native reentry,
nested public STEP and full collect, threshold/hook restoration and errors.
Nested callbacks run under that same owner and unwind in order. Saving whether
PAUSE was previously present is therefore sufficient: only the outer scope
which first set it clears it. A peer cannot hold an overlapping foreign-owned
PAUSE scope. Public RESTART inside a callback clears only STOP and retains
PAUSE; a callback-issued STOP survives outer PAUSE cleanup. Full collection
while a finalizer is active retains its existing deferral; its normal public
API return still performs RESTART but does not clear PAUSE. Nested full
collect in these controls is not claimed as a completed collector cycle.

This proof depends on retaining the existing physical callback owner. Moving
callbacks outside that owner requires a new pause ownership protocol. The
existing finalizer pause can last arbitrarily long and remains a separate
blocking protocol limitation; representing it explicitly does not make it
nonblocking.

## Exact overlap and production controls

The original `t-stop-first-attach.c` and
`t-stop-first-attach-restart-v2.c` are unchanged copies from the preceding
package. The real first-live STOP overlap remains rejected in normal, strict
and ASan builds. Sequential RESTART after that overlap completes three actual
cycles within the original v2 16,384-allocation bound. The prior 8,192-bound
calibration failures are retained in the older package, not rewritten.

The unchanged `t-restart-first-attach.c` stops real GCC/x64 attachment after it
captures stopped global MAX but before the emitted MOV into the saved MT
threshold. Its TF handler verifies the actual base register, displacement and
source register. Public RESTART completes, then the delayed real attachment
store overwrites both pacing thresholds with MAX. The previous-veto control
reports stopped and starts/completes no cycle within 8,192 ordinary TNEW
allocations, returning 43. Candidate2 and strict report running and advance
actual starts/completions from 4/4 to 6/6, then clean up normally, returning 0.
The intermediate candidate1 also passed this schedule and remains recorded.

The separate `t-restart-last-detach.c` pauses the real GCC/x64 final-child
global-threshold MOV after live becomes zero and the saved stopped MT value
has been loaded. It verifies RDI is g, the displacement is gc.threshold and
RAX is MAX. Main RESTART completes before this real delayed store resumes.
Control loses running and remains 2/2 after 8,192 TNEW allocations, returning
43. Candidate2 and strict preserve running despite global MAX, complete two
actual cycles (2/2 to 4/4), and return 0. All cases detach/close normally.
The fixed-register RESTART overlap fixtures are GCC-specific and were not run
on clang/ASan. ASan did run the separate first-live STOP overlap and the other
production controls; do not combine those into an exact ASan RESTART claim.

`t-auto-restart-numeric-max.c` uses one ordinary stack-anchored userdata to
make observed total exactly 13,107,200 bytes, then public SETPAUSE(2^30) and
RESTART(-1). The computed pacing threshold is exactly MAX (2^47). No threshold,
phase or counter is fabricated. Candidate1 reports stopped and completes no
cycles after 8,192 TNEW, returning 45. Candidate2, strict and ASan report
running and complete two actual cycles, then free normally. This is why the
public operation, not the pacing number, selects the flag update.

`t-auto-finalizer-controls.c` runs 16 real callback schedules in each of
control, candidate2, strict and ASan: peer absent/present, inner error absent/
present, outer error absent/present and outer STOP absent/present. All 64
return 0. It creates ordinary unreachable userdata with real C __gc callbacks,
then drives bounded public STEP calls. Actual nested callback depth and active
owner count reach two. A real foreign attached peer attempts 16 public STEP
calls and full collect while the first owner is in its callback; the peer
cannot dispatch the second callback. Public STOP/RESTART, nested callback
STOP, nested full collect, and errors preserve the expected flags. Final
checks require zero callback depth/active/owner, restored hook state, correct
Lua stack top and the latest public STOP result. The control logs its prior
query behavior and uses its versioned old-state oracle. Intentional finalizer
error stderr is expected and is distinct from sanitizer reports.

The unchanged production string matrix is repeated in normal, strict and
ASan, peer 0/1, configured workers 0/2, explicit/automatic. Eighteen of 24
runtimes return 0. Worker-zero automatic controls complete 18 actual cycles
over six rounds and 49,152 ordinary fixed-ring TNEW allocations, including
with a persistent real child. All 24,576 newly created unreachable strings
remain interned despite those completed automatic cycles. Explicit sole-main,
worker-zero collection reclaims them; either a persistent peer or a configured
pool retains them through explicit completed cycles. The unchanged fixture
accounts exact string counts minus private credits, runtime heap bytes, known
body bytes and canonical identity of 32 anchored live strings. No RSS argument
is used. Disabling workers, joining the child and explicit sole-main cleanup
restores the original string set.

The other six matrix runtimes are automatic worker-two cases which admit the
request, complete one cycle and miss later SWEEP completion within the fixed
262,144-table bound, returning 2. Sixty-six unchanged boundary/sequential
controls cover TNEW, TDUP, FNEW, string.char, string.rep and numeric C-string
conversion, STOP/restart, first/last attachment and real native entry/return.
Every worker-zero case completes; 33 worker-two cases return 2 at the later
SWEEP bound after successful admission. Twelve active-STOP controls add six
worker-zero passes and six worker-two completion failures. STOP does not
promise to cancel active work; the controls preserve that distinction.

## ISRUNNING compatibility counterexample

Candidate2 returns false whenever STOPPED or FINPAUSE is set. The old query
used MT/global thresholds and could return true during a temporarily paused
callback with a live peer, or after callback RESTART. Candidate2 now exposes
the actual temporary automatic-admission pause consistently, independent of
attachment geometry. This is a real public behavior change.

The untouched `tests/t-m8-finalizer-spawn-live.lua` creates a child in an actual
finalizer. That child immediately records ISRUNNING, and the parent later
requires its recorded answer to be running. The child may run before callback
cleanup, while the new PAUSE is still set. Candidate2 JIT, strict interpreter
and ASan interpreter fail the existing line-33 expectation; the other three
new candidate schedules happen to pass. Previous-veto control passes both
interpreter/JIT modes. These scheduling-dependent passes do not remove the
counterexample and are not used to certify the query change.

The separate `t-finalizer-spawn-query-overlap.lua` makes the timing explicit
using ordinary channels: the actual callback waits for its newly spawned
child's query before returning. After callback cleanup, the parent releases
the child to query again. In all cdata/userdata and full-collect/step modes,
control reports during=running, after=true, child_after=true. Candidate2,
strict and ASan report during=stopped, after=true, child_after=true. All eight
versioned diagnostic interpreter/JIT runs pass. The original fixture and its
failed outputs remain unchanged. Resolve the intended query contract before
integration without restoring threshold authority to automatic admission.

Other existing finalizer checks pass: finalizer callback state ownership,
close-time cdata/userdata chains/errors/nested collection, and FFI registration
and clearing under threads. All control/new production variants run the two C
tests and both Lua modes for FFI finreg and spawn-live: 21 of 24 pass, with
exactly the three spawn-live query failures above.

## Existing safety findings retained

The new four-helper safety run contains 28 runtimes: 25 pass, two abort and
one times out. It retains the original interpreter hard-assist assertion at
`t-gc2-interp-hard-check.c:230` and accounting assertion at
`t-gc2-alloc-account.c:929`. Matching failures already exist in the earlier
immutable packages. Their expectations were not changed here.

The unchanged closure-construction fixture times out after 60 seconds in both
new candidate helpers and the exactly matched four-helper previous-veto
control. New three-second debugger samples place both at the injected
`t-func-construction-anchor.c:193` path through func_finduv_nothrow and explicit
lj_gc2_collect_active into SWEEP/bridge progress. This precedes the later
concurrent-open-upvalue subtest. The exact stack tops differ and are preserved;
the diagnostic debugger exit 0 is not a fixture pass. Earlier six-helper
matched failures and the earlier four-helper pass remain in prior packages.

The repaired scheduler fixture copied from 0e2119e4 has SHA256
`f476733106d7e2552c9e47757b35f1c1dc78cf8b3cb0d47c17f17ed58fe70dca`.
It passes on the new helper runtime. The first compile lacked the tests include
directory and failed; its output remains separate. The corrected compile/run
uses `SCHEDULER_RUN_SUFFIX=-v2`. The parent's separate scheduler review supplies
the publication-race diagnosis and test repair. The old wait_ssb_empty failure
is not erased or explained away by this pass, and this test repair does not
resolve the unrelated worker-two SWEEP completion bound.

The old IDLE reclaim regression fixture's 60-second timeout and real wait stack
remain at `/tmp/lj-gc-auto-admission-20260905-h7ntx71p/` in
`control-idle-reclaim-gdb.*`. Main was blocked in lj_tab_wait_l through rooted
ITERN at fixture line 318, while a peer held real idle reclamation entry. That
fixture was not modified or rerun here; this patch does not claim to resolve it.

## Review, totals and reproduction

The parent's independent source-only review is copied under
`independent-review/`. Its original is
`/tmp/lj-gc-auto-control-proof-20260905-31ki7iox/review.md`, SHA256
`1c1c459e5b1db917baa058b680fd24064097c7aa1254bfb9014923ce0395ac8c`;
the original manifest SHA256 is
`10fa8c7c0b448ad6f7152d221f3ebf64a7e9fbcb670717832a3517be60700f80`.
It found no source blocker for the bounded protocol and explicitly required
final runtime checks. It predates the final spawn-live compatibility failures;
do not present the source review as approval of those runtime outcomes.

`summary.json` groups 247 normal runtimes: 192 exit 0, 45 bounded worker-two
incompletions (exit 2), two retained lost-RESTART controls (43), one retained
candidate1 numeric-MAX failure (45), three query compatibility failures (1),
two helper assertions (-6) and two closure timeouts. It also records two
separate interrupted debugger runs. All 61 ASan runtimes have no sanitizer
report; that includes expected bounded incompletions and the query failure,
not only exit-0 runs. Every command and stderr result is preserved.

From this directory, the committed source can be reproduced by extracting
`runtime-source.tar` in a new directory, applying
`automatic-control-full.patch`, and using the exact desired `*-build.json`
command. Fixture compile/link and runtime argv, cwd and environment are in the
corresponding `*-results.json`; binaries/inputs are hashed in `*-identities.json`
or `*-inputs.json`. The scripts are executable via Python and accept a tree
variant: build.py, run.py, controls.py, controls-v2.py, boundary-overlaps.py,
finalizers.py, existing-finalizers.py, spawn-query.py and repaired-scheduler.py.
Active STOP uses controls-v2 cases 4-0-0-0, 4-0-0-2, 4-0-1-0 and 4-0-1-2.
Do not rerun scripts against frozen output names; copy the package or choose
the supported suffix first. `summarize.py` derives the counts from saved logs.

Earlier immutable evidence remains in
`/tmp/lj-gc-auto-stop-overlap-20260905-y4h4cc8a/`, manifest SHA256
`ff02877adb7dafe854fbc8173e71ae3a3b0b2a1d6d773226bf182826454c90d8`,
and in the initial admission and original string-retention packages cited
there. This package keeps those STOP failures, lost RESTARTs, bounded-cycle
counterexamples and actual waits intact. Its evidence manifest copies the
handoff, patches, fixture sources, logs, identities, final six runtime sources
and independent review, and records source archive/build hashes.

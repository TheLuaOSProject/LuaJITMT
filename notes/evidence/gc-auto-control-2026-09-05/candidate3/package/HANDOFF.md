Candidate3 passes the requested public-control and finalizer compatibility
checks while keeping temporary finalizer suppression in every internal
automatic-admission path. The original finalizer-spawn-live fixture is
unchanged and passes normal, assertion and AddressSanitizer builds with JIT
both enabled and disabled. The exact STOP/RESTART attachment schedules and the
numeric-MAX control also pass within their recorded platform-specific bounds.

This package is ready for the parent's bounded control/admission integration
review. It does not establish release readiness or a fully nonblocking
collector. The 151 recorded runtimes include 27 preserved configured-worker
SWEEP completion failures, distinct from request admission. The earlier helper
assertions, construction waits and IDLE reclaim wait remain separate evidence.
The parent is validating additional combined stock/JIT/FFI behavior and any
separate closure repair in its own isolation. Those results are not claimed
here, and no separate runtime repair is included in these frozen sources.

## Final patch and source identity

The exact committed archive base is
`0e2119e4a36a7034f1f6e1115849677a4ce4359e`.
`runtime-source.tar` SHA256 is
`c37911c0bf056bedb57df96b89cdf95381b408eee105153c7fdaa839062e25d5`.
`source-identity.json` records all 815 archived files. This agent changed no
shared source/build and made no commit.

The full six-file patch is `automatic-control-candidate3-full.patch`, SHA256
`5f8a22b6bc1ae8956f63844cc0ad8bd63b093db98070078eeb391ee5e48ed9a1`.
The two-file delta over candidate2 is `automatic-query-candidate3.patch`, SHA256
`c82c6a22f327b6259dd877390fda8f771463a7d3278df84121e6c7640f02df5e`.
It adds `lj_gc_auto_enabled` in lj_gc.h and changes LUA_GCISRUNNING to call it
in lj_api.c. The other four modified runtime files are byte-identical to
candidate2. `candidate3-source.json` records all six exact source hashes.

All 815 sources were compared in the normal, assertion/API-check and ASan
trees; each has exactly the same six intended changes. Twelve prior fixture
sources and all four existing finalizer tests match their previous bytes.
`validate-source.py` and `source-validation.json` record these comparisons.
The full patch was also applied without fuzz to a fresh extraction and all six
resulting source hashes matched the built inputs (`patch-apply.json`).

The normal executable SHA256 is
`a399c29f009c8deecc04de97cb0310205be2c9d1cc305e0a66c70eeb41757013`;
its static archive is
`abd9ede84ae1399adfd27c11a5079f69d5060a6fd45e7cc9f80cf01b7438eb97`.
Strict archive SHA256 is
`80e8552e12423b6371007fcdadeefc4119b144bb202eab86421777dcb10cbfc8`;
ASan archive SHA256 is
`4e66f9d450ad2585fbcab6d90ec671fb5c4914bfca4db1a3d748a490e4441cd3`.
Each `*-build.json` retains exact make argv, cwd, return code, duration and
executable/archive hashes. No production tree enables test-helper macros.
Strict enables assertions and API checks. ASan uses clang, target-only ASan,
`-O1 -g`, assertions/API checks and `detect_leaks=1:abort_on_error=1`.

## Chosen public query contract

ISRUNNING now reports the explicit public STOPPED bit only. A callback, an
existing peer and a newly attached child therefore observe the same logical
STOP/RESTART state. The new `lj_gc_auto_enabled` acquire-loads auto_flags and
tests STOPPED; its only new caller is LUA_GCISRUNNING. This query schedules no
work and changes no state.

The existing internal `lj_gc_auto_running` still requires all flags to be zero.
It includes FINPAUSE, the temporary pause owned by an actual finalizer scope.
Raw automatic request creation, the C pending-request predicate, the actual
automatic driver and the x64 pending-request branch all retain FINPAUSE.
Public RESTART does not clear it. Public STOP still publishes its bit before
pacing stores; RESTART publishes pacing before clearing only STOPPED. No
threshold-derived logical-control test was restored.

This deliberately changes the old sole-main callback query too: when no
public STOP is in effect, ISRUNNING is true during a callback even though
automatic admission is temporarily suppressed. The older threshold-based
behavior could report false for a sole-main callback and true for its spawned
peer. The existing fork's spawn-live fixture requires the peer to see logical
running. The parent chose one consistent public setting across actors. This
is not a claim of exact upstream callback behavior.

Candidate2 exposed FINPAUSE through ISRUNNING and failed three of six original
spawn-live schedules. Its deterministic diagnostic proved that the child
sampled before callback cleanup. Candidate2 and all 247 results, its old query
oracles and old independent review are immutable at
`/tmp/lj-gc-auto-control-20260905-qs673ryl/`; its evidence manifest SHA256 is
`5bdc2b9431e12a3d769d579b2ab09da3e68074b72258650b9c1fe81274351300`.
The failures are not overwritten, relabeled as passes or hidden by changing
the original existing test. The new explicitly named oracle changes are
recorded in `versioned-query-oracles.patch`.

## Runtime validation

All counts below are real subprocess runtimes, excluding compilation and
disassembly commands. Every argv, environment, cwd, stdout, stderr and return
code is saved in `*-results.json`; source/library/executable identities are in
`*-identities.json` or `*-inputs.json`. `summary.json` is generated from those
saved outputs by `summarize.py`.

| Group | Runtimes | Result |
| --- | ---: | --- |
| Nested, foreign-owner and throwing finalizer controls | 48 | All pass |
| Unchanged existing finalizer tests | 18 | All pass |
| Deterministic spawned-child public-query overlap | 6 | All pass |
| First attachment STOP/RESTART and numeric MAX | 11 | All pass |
| Exact last-detachment RESTART overlap | 2 | Both pass |
| Explicit/automatic, peer 0/1, worker 0/2 string matrix | 24 | 18 pass; 6 worker-two completion failures |
| Sequential STOP/attachment/native controls | 30 | 15 pass; 15 worker-two completion failures |
| Active STOP controls | 12 | 6 pass; 6 worker-two completion failures |

Total: 151 runtimes, 124 exit 0 and 27 exit 2. There are no new query/assertion
failures or timeouts in this selected matrix. All 49 ASan runtimes contain no
sanitizer report. Some intentionally throwing finalizer controls produce error
messages, and the ASan count includes bounded worker completion failures; no
absence of a sanitizer report is presented as an exit-0 result.

`t-auto-finalizer-controls-v3.c` is a new explicitly versioned copy of the
prior C control; the old source remains copied unchanged. The only new oracle
distinction is that internal admission remains false while the public query
matches STOPPED. It still checks actual FINPAUSE/STOPPED flags, physical owner,
nested depth, active owner count, Lua stack top and restored hook state.
Sixteen schedules per build cover a real peer, inner/outer errors and outer
STOP in all combinations. Ordinary userdata __gc callbacks enter under real
public STEP. Same-owner nested callback depth reaches two. A foreign attached
peer attempts 16 public STEP calls and a full collect while the outer callback
is live; it cannot dispatch the other callback. Callback and peer RESTART keep
FINPAUSE, inner STOP survives nested cleanup, and outer STOP survives outer
cleanup. This also directly checks sole-main callback queries. Nested full
collect retains its preexisting finalizer deferral and is not counted as an
actual completed cycle.

`t-finalizer-spawn-query-enabled.lua` is the new deterministic public oracle.
An actual cdata/userdata callback spawns a child, waits through an ordinary
channel for the child's query, then returns. Only after cleanup does the
parent release the child for another query. Full-collect and public STEP
variants all report during=running, after=true and child_after=true in normal,
strict and ASan, with JIT on and off. The prior stopped-during-callback oracle
remains as `t-finalizer-spawn-query-overlap.lua`; it was not overwritten.

The original `t-m8-finalizer-spawn-live.lua` is unchanged and passes all six
modes. Existing `t-m8-finalizer-state.c`, `t-m8-close-finalizers.c` and
`t-ffi-gc-finreg.lua` also pass, preserving caller-state ownership, close-time
callback chains/errors/nested collection and concurrent FFI finalizer
registration/clearing.

The exact first-live STOP overlap remains rejected with no new cycle, even
when stale finite MT pacing is visible after the live count changes. Sequential
RESTART after that overlap completes three cycles within the unchanged v2
16,384-allocation bound. The exact lost-RESTART fixture pauses an actual GCC
attachment MOV after it captures stopped MAX; RESTART completes before the
delayed real store. Candidate3 and strict preserve ISRUNNING and complete two
actual cycles within 8,192 ordinary TNEW allocations. The unchanged exact
last-detachment fixture does the same around the real delayed global threshold
MOV after live reaches zero; both GCC variants pass with two actual cycles.

The emitted-register RESTART overlap fixtures are GCC/x64-specific and were
not run on clang/ASan. ASan runs the separate first-live STOP overlap, numeric
MAX and sequential first/last controls. Do not present those as an exact ASan
lost-RESTART overlap test. The numeric-MAX fixture uses one ordinary anchored
userdata and public SETPAUSE/RESTART to produce a pacing number equal to MAX;
all three builds remain logically enabled and complete two actual cycles.
The negative old-veto RESTART controls and candidate1 numeric-MAX failure stay
in the immutable predecessor packages.

The unchanged string-retention matrix has a fixed live ring and 32 canonically
anchored strings. Worker-zero automatic cases, with and without a real
persistent peer, complete 18 actual cycles over six rounds and 49,152 ordinary
TNEW allocations. The 24,576 new unreachable strings remain retained after
these completed automatic cycles. Explicit sole-main, worker-zero collection
reclaims the string bodies. A persistent peer or a configured worker pool
continues to exclude physical reclamation through explicit completed cycles.
Exact string counts subtract private credits; runtime heap bytes and known
body bytes are recorded. No RSS argument is used. All measurements end with
normal worker disable, peer join and explicit sole-main recovery of the
original canonical string set.

Automatic worker-two matrix cases admit requests, complete one cycle and miss
the later SWEEP completion target within the unchanged 262,144-table bound.
Sequential STOP/restart, first/last child and actual native-return cases retain
their successful admission/control assertions but hit the same later bound
with two workers. Active STOP forbids a new automatic cycle without promising
to cancel existing work. These 27 exit-2 outcomes are preserved as incomplete
progress, not passes. The six other request boundary kinds were validated in
candidate2; their source is unchanged here. Candidate3 repeats the promised
control/matrix cases while the parent handles wider combined regressions.

## Source proof, review and remaining scope

Candidate2's internal control/ownership proof still applies. Only initialization
clears the whole flags byte. Attachment, detachment, finalizer-threshold and
pacing stores never write it; delayed stores can no longer undo public control.
The API operation selects STOP/RESTART rather than a numeric threshold. The
physical finalizer owner covers the entire FINPAUSE interval, including its
first update, nested callbacks, nonthrowing pre-pcall setup, protected callback
errors and current-state/hook/threshold restoration. Only the outer owner
clears FINPAUSE, before ERRFIN delivery, retaining any public STOP. The exact
ownership/unwind reasoning and baseline counterexamples are in the preceding
handoff and both frozen source reviews.

The superseding independent source-only review is copied to
`independent-review/review.md`. Its original is
`/tmp/lj-gc-auto-query-proof-20260905-a8tkbct0/review.md`, SHA256
`2deb4c50ac696f607213fb89210fe3eff239bf9261f4750912d168a547f11385`;
the original manifest SHA256 is
`939a078fefb57fed7795ae209e7358c1aaef64fc044d484fbc16fa8f655a5595`.
It approves the exact query delta for final validation and explicitly
supersedes the old public-query decision without changing candidate2 evidence.

STOP and FINPAUSE are sampled at automatic invocation entry; they do not
cancel an invocation authorized before the update or create atomic MARK
arbitration. The existing synchronous driver can wait for root/action/native,
phase and peer protocols. Native threshold/hard-cadence geometry is unchanged.
The query is observational and grants no new explicit STEP/COLLECT authority.
No safety gate or string reclamation policy was removed. Remaining string
canonical identity and native-borrower proofs are still required before any
concurrent physical reclamation change.

The earlier helper hard-assist/accounting failures, matched closure-construction
timeouts and real IDLE reclaim wait are not rerun or repaired by this query
refinement. Their exact failed outputs and debugger stacks remain in the prior
immutable packages. The repaired scheduler fixture passed on candidate2 and
the parent owns its broader validation. The separate closure agent is
investigating collector defer behavior; no proposed change from that work is
part of candidate3. Preserve these distinctions when applying the patch.

## Reproduction and preservation

`canonical-gate-proposal.md` and `.json` identify the smallest proposed
permanent fixture set, exact argv/environment and platform exclusions for
`m3_gc2_auto_control`. No proposed fixture embeds an absolute package path.
This is a reviewable registration proposal; the parent owns shared suite
integration and execution of the complete proposed gate.

Extract `runtime-source.tar` in a fresh directory, apply the full patch with
`patch -p1 --batch --fuzz=0`, then use the exact desired `*-build.json` command.
`build.py` accepts candidate3, strict or asan. Production run.py covers all
eight explicit/peer/worker cases. This package's controls.py runs modes 1 and 3
for peers 0/1 and workers 0/2, plus mode 2 with peer 1 and workers 0/2.
controls-v2.py runs 4-0-0-0, 4-0-0-2, 4-0-1-0 and 4-0-1-2. Other runners are
boundary-overlaps.py, last-detach.py, finalizers-v3.py, existing-finalizers.py
and spawn-query-v3.py. Exact compile/link and runtime argv are recorded even
where a copied earlier runner is retained but was not executed here.

Do not rerun scripts over frozen output names. Copy the package or select a
supported suffix first. `predecessor.json` identifies candidate2's immutable
archive and all 247 results. The final evidence manifest copies this handoff,
patches, all fixture sources/logs/identities, six final runtime sources and the
superseding source review, with build and archive hashes. Earlier evidence is
preserved by reference and is not reclassified as candidate3 validation.

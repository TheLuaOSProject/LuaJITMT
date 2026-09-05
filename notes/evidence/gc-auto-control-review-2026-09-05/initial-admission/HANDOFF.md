**Review status: not ready to integrate.** Independent source review found a
potential new STOP violation when first attachment publishes `mt_live` before
initializing the saved MT threshold. Sequential controls below do not cover
that overlap. A controlled reproduction and repair are in progress in a
separate package named by `/tmp/lj-gc-auto-stop-overlap-current`; the three-file
candidate and all existing evidence remain unchanged.

The three-file candidate repairs admission of a published automatic IDLE
request at existing Linux x64 GC-safe interpreter and C boundaries. It does
not establish asynchronous, nonblocking collection or fix the separate
configured-worker SWEEP completion problem. No shared source/build was edited
and no commit was created.

The frozen base is `597b8705208957ade8465416da30976ab9b52195`, captured from
committed shared HEAD before this experiment. Parent receiver/optimizer work
continued independently; the last observed shared HEAD was `5c455f20...`.
`runtime-source.tar` and `source-identity.json` identify every captured input.
The earlier completed baseline at
`/tmp/lj-string-retention-baseline-20260905-424nvcfy` remains untouched.

The candidate is `candidate.patch`, SHA256
`f632c7f26051ffebb55fde65291a304b91a46df0371cf47f86aa1783efdf5cd9`.
The authoritative source files are:

- `candidate/src/lj_gc.c`: `d0e8608a261b823f21b037aa73f4f29dac3059ee3e741286d836c686e7498e9d`
- `candidate/src/lj_gc.h`: `f465a7706da2f3f7fc52f0b40054462d0a6368c3e5dec0abcd9d990c6680cfd5`
- `candidate/src/vm_x64.dasc`: `d445a4ffa214242bd5efc753c3cb737e90509b66c9f52bd95eff4c358b6e77ab`

The original first-child bridge in `lib_threading.c:895` saves the ordinary
threshold in `mt_gc_threshold`, then writes `LJ_MAX_MEM` to `gc.threshold`.
`threading_gc_leave` restores the saved value only at the last live child's
departure. Public stop/restart and a newly published GC2 request update the
logical MT threshold while children remain live. A request therefore leaves
the ordinary global threshold at MAX. Allocation checkpoints flush local
accounting debt at 32 KiB, publish `cycle_leader`, and may assist an existing
MARK/WEAK phase; they do not initialize MARK from IDLE. IDLE workers likewise
do not initialize MARK.

Before the patch, the C predicate checked only global threshold/hard pressure;
the x64 predicate additionally required sufficient local debt at normal hard
pressure. The flush could consume that debt before the next VM check. Even
when the C hard predicate admitted the helper, `gc_step_assist_top` called the
automatic phase driver only when the ordinary global threshold was due.
Consequently an IDLE request could remain durable while neither route drove it.

The patch adds `lj_gc_pending_auto_request`: nonzero cycle token, IDLE phase,
and the existing logical-running test including its last-child recheck. The
C predicate invokes that slow check only after observing a nonzero token.
The x64 predicate mirrors it with TSO acquire loads, including stop state and
last-child restoration, before the hard-assist cadence. At the already safe
helper entry, `gc_step_assist_top` enters the existing automatic driver for a
still-pending request as well as ordinary threshold debt.

No new entry is added inside raw allocation accounting, object construction,
native execution, or a worker's IDLE loop. Existing x64 TNEW/TDUP paths still
save PC/base and use `lj_gc_step_fixtop`; fast functions retain their existing
stack handling, and C callers retain the original GC-safe call sites. The
automatic driver rechecks logical running and request/phase before stepping.
Worker claims, typed activation veto, native/JIT admission, SMR exclusion,
root/action handshakes, and string reclamation admission are unchanged.
The nonzero predicate may sample the brief IDLE GCSCAN close sentinel;
`gc2_mark_begin` explicitly rejects that sentinel before any phase mutation.
An independent source review is checking whether filtering that sentinel at
the scheduling predicate would be a worthwhile additional refinement.

The common x64 no-request path gains a token compare and branch. C checks gain
a token load/branch after existing threshold and hard checks fail. There is no
throughput benchmark in this package, so no percentage performance claim is
made. The demonstrated improvement is bounded-heap workload progress with a
persistent peer when no GC worker pool is configured.

The production matrix reuses the original baseline fixture unchanged:
`t-string-retention.c` plus `peer-control.lua`. It creates 32 rooted 64-byte
canonical strings, then six rounds of 4,096 ordinary unreachable strings.
Public interning and peer checks verify canonical pointer/content identity
while the anchors stay rooted. Count accounting subtracts actual unused
main/peer string credits; workers have no Lua stack and do not intern.
Each string's accounted body size is 100 bytes, so 24,576 churn strings account
for 2,457,600 body bytes. These are object counts/runtime-accounted bytes, not
RSS or claims about physical mappings. Expanded string-table headers persist.

The automatic interval has no explicit collect/step, internal collector hook,
or fabricated phase. Its interpreter loop overwrites a fixed 32-table ring,
up to 262,144 table allocations. After each 4,096-table burst it reads the
actual completed-cycle counter and permits eight ordinary 2 ms native sleeps.
Success requires three new SWEEP-to-IDLE transitions per round. The predicate
does not require the sampled current phase to be IDLE, since another real
cycle can already have started. Full collections occur only in explicit
comparison cases and setup/cleanup, outside automatic measurement intervals.

| Interpreter automatic case | Unchanged current-head control | Candidate normal / assert / ASan |
| --- | --- | --- |
| peer 0, workers 0 | 18 starts / 18 completions in six rounds | 18 / 18 in every build |
| peer 1, workers 0 | 0 / 0; pending IDLE request after 262,144 tables | 18 / 18 in every build; 49,152 tables total |
| peer 0, workers 2 | 1 / 0; incomplete SWEEP | 2 / 1 in every measured build; incomplete SWEEP |
| peer 1, workers 2 | 0 / 0; pending IDLE request | 2 / 1 in every measured build; incomplete SWEEP |

Worker cases stop after the first failed three-completion bound and return 2.
The second started cycle remains in SWEEP after 262,144 table allocations;
the candidate measurements account for about 27.7 MB more heap. This is
evidence of poor bounded-workload completion, not proof of permanent deadlock.
Worker asynchronous progress counters are real and advance substantially;
that work counter is not substituted for completed cycles. Request admission
is repaired in the peer+worker case, and the independent later problem remains.

All automatic cases retain ordinary unreachable string bodies, including the
completed sole-main and peer-only cases. All candidate explicit comparisons
complete 12 actual cycles: sole-main/no-worker reclaims all 24,576 churn bodies;
any persistent peer or configured pool retains them. Every matrix process
disables workers, joins its peer, then completes ordinary explicit collection
and returns to the original 300-string count. This is deliberate preservation
of the existing string gates, not a string reclamation repair.

The additional production admission fixture is `t-auto-controls.c`, which
reuses the read-only accounting routines by including `t-string-retention.c`,
plus `control-boundaries.lua`. No test-helper macro is used in its runtime
builds. It creates a real request with ordinary string allocation and stops
immediately upon its publication. In every measured publication, local debt
is exactly zero and global allocation debt remains below the hard threshold.
With a peer, global threshold remains MAX while logical GC is running. Thus
successful admission cannot be explained by residual local hard debt or the
old ordinary threshold path.

The six entry kinds are TNEW, TDUP, `string.char`'s x64 fast-function check,
`string.rep`'s C check, public number-to-string conversion's C check, and
FNEW's C fixtop check. The bytecode dump in
`control-boundaries-bytecode.stdout` verifies the table/closure opcodes.
All six admit a real cycle with peer 1 and workers 0 or 2 in normal, assertion,
and ASan builds. Every corresponding unchanged peer control misses admission
and returns 3. Every no-pool candidate also completes the next three actual
cycles. Worker-pool candidates admit but return 2 after the unchanged later
automatic bound; their subsequent completions vary between 0 and 1.

The same controls cover public STOP with an existing request, 256 stopped
table allocations plus fast/C string calls, RESTART, and a subsequent TNEW.
They verify no new request/start/completion while stopped and preservation of
the exact pending token. They also cover stopped last-child detach followed
by first-child reattachment: both global and logical thresholds remain MAX,
the same pending token survives, and restart permits admission. All of these
admission/stop assertions pass in each candidate build, including pool cases.

The native-return case executes real `threading.sleep(0.002)` before the next
allocation boundary. A GNU linker wrapper only observes the actual
`lj_native_enter`, calling its original implementation unmodified; it verifies
published native state. After return, native state is zero and the request
remains pending until the next TNEW, which then admits it. This does not claim
native return alone is a GC progress driver.

`t-auto-controls-v2.c` adds active-cycle STOP/RESTART. Immediately after the
first boundary has started MARK, public STOP precedes 4,096 ordinary fixed-ring
table allocations and 16 native sleeps. No new cycle/request starts, and the
logical running query remains false. Active worker work may still finish the
already started cycle, consistent with the existing contract. Public RESTART
resumes the same automatic allocation driver. The no-pool cases complete;
pool cases again retain the later SWEEP failure. Before explicit cleanup,
last-child detach restores the exact sampled saved threshold in all builds.

There are 128 final production/control runtime processes in `summary.json`:
28 string matrix cases, 88 single-boundary/stop/attachment/native controls,
and 12 active-stop controls. They include 34 ASan runtime processes with
`detect_leaks=1:abort_on_error=1`, no suppressions, and empty stderr. Nine
earlier successful no-pool calibration controls are additionally preserved in
`candidate-controls-results.json`. Exit 2 means incomplete automatic progress;
exit 3 means missed admission; those counterexamples are not treated as passes.

Seven build trees are retained. `control` and `candidate` use default builds;
`strict` and `asan` are production assertion/APICHECK builds, without helper
macros. `helpers` and `controlhelpers` use identical GC2/trace/table/function
helper plus assertion flags, with only the three candidate files differing.
`extrahelpers` adds userdata/string helper flags for their existing fixtures.
All successful build commands and binary identities are in `*-build.json`.
`final-source-verification.json` verifies every captured source input in all
seven trees, exactly the three intended differences, and current binary hashes.
The ASan target has ASan references; host minilua/buildvm do not.

Existing safety coverage has 37 runtime processes across candidate and matched
controls. Candidate successes include atomic pacing, JIT hard checks,
activation veto, worker scheduler, MARK-close progress, native MARK and SWEEP
cooperation, root-pending publication races, function/userdata construction
roots, spawn native STOPREQ, sole string reclamation, all 11 retained native
root/completion modes, and active-thread roots/workers/finalizer-peer-collect
scripts with JIT on and off. The unchanged tests were not weakened or edited.

Three substantive existing safety failures are preserved and reproduce on
the unchanged committed control with identical helper/assertion flags:

- `t-gc2-interp-hard-check.c:230`: hard-only TNEW enters the hard check but
  does not increase the assist-run count at that test's assertion.
- `t-gc2-alloc-account.c:929`: exact remembered-filtered delta is not `+4`.
- `t-jit-idle-reclaim-entry`: 60-second timeout. The bounded control gdb probe
  interrupts at the frozen-reclaimer test's `closed IDLE ITERN shadow`, line
  318. Main waits in `lj_tab_itern_rooted -> lj_tab_next_rooted -> lj_tab_wait_l
  -> lj_thr_retry_yield`; the other pthread remains in `gc2_idle_reclaim_enter`.
  Commands, output, and stack are in `control-idle-reclaim-gdb.*`.

Two initial harness omissions are also retained: missing `thread_harness`
Lua path caused the first active-thread-root scripts to exit before testing;
missing `LJ_UDATA_TEST_HELPERS` caused the first userdata fixture compile to
fail. Corrected runs pass in `helpers-safety-lua-path-results.json` and
`extrahelpers-safety-results.json`. The first unsuccessful candidate build
used the incorrect field spelling `gc.mt_threshold` in generated offsets;
the compiler rejection, source, and build record are retained separately.
The successful candidate uses the real field `mt_gc_threshold` and has not
changed since its first successful build.

To reproduce without overwriting retained runtime logs, execute from this
package with a fresh suffix, for example:

```sh
RETENTION_RUN_SUFFIX=-review python3 run.py candidate 0-1-0 0-1-2
RETENTION_RUN_SUFFIX=-review python3 controls.py candidate
RETENTION_RUN_SUFFIX=-review python3 controls-v2.py candidate 4-0-0-0 4-0-0-2 4-0-1-0 4-0-1-2
SAFETY_RUN_SUFFIX=-review python3 safety.py helpers t-gc2-jit-hard-check t-gc2-jit-mark-coop t-gc2-jit-sweep-coop t-gc-root-pending-race t-safepoint-native-root-hold
```

The runners deliberately return nonzero when any runtime case returns 2/3 or
fails; inspect recorded per-case outcomes. `build.py VARIANT` describes all
exact build flags. For clean reproduction, extract `runtime-source.tar` into
a new tree, apply `candidate.patch`, and use the command from the matching
build JSON rather than rebuilding an evidence tree in place.

The next implementation scope should remain request admission and its
production regression controls, after independent review of this candidate.
Investigate worker-pool SWEEP completion separately with the retained fixed
live set and actual completion counters. Do not remove string/phase/worker
gates to make it pass. In particular, `gc2_step_auto` still synchronously calls
`lj_gc2_step_explicit(L, 1)` within the existing 64-unit budget, and phase work
can call root/action handshakes. A unit-count limit does not prove bounded
wall time or independence from another actor. Resumable handshake/progress
ownership is a later proof obligation; none of the results here establish the
full lockless/nonblocking GC goal.

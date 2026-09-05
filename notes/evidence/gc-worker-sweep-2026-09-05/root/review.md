# Combined worker SWEEP review

The accepted combination applies worker candidate2 to the exact runtime of
eb8a5b2f (constructor deferral and between-TG fairness), archived at 4b4ed7c2.
Only src/lj_gc2.c changes among 225 runtime/generator inputs. Later commits
24e2e2b1 and 6490b127 change fixtures/evidence only. Shared source before final
promotion is identical to the default, strict and ASan isolated combinations.

## Source decision

The boundary helper retains pre-claim and post-claim admission, RESET_ALLOC,
the exact semantic snapshot, current physical actor's SSB, graph/recovery/
rescan/NEEDSCAN/mark checks, weak/native/JIT exclusions, bounded spine prune and
the irreversible READY publication. Its old public void wrapper remains.
Result classification occurs before releasing the same worker claim and
compares only the same SWEEP generation. A NULL cursor is normalized to the
head slot, so first refusal cannot fabricate progress. Actual unlink, cursor,
EOF, snapshot, READY or preparation advance permits another scheduler turn.
Constructor deferral wins over such an advance; unchanged/blocked work uses
the existing interruptible deferred backoff. Graph-only advancement may
conservatively back off. Synchronous handshake and whole-chain EOF costs are
unchanged and explicitly remain open work.

The worker closes its actual startup native depth before detach. An already
consumed action retains the old exact poll-completion hold. A request arriving
after native depth becomes zero cannot borrow private teardown through the
parked-native route. Candidate1's real teardown overlap is not reclassified
as a passing control; candidate2 includes this required lifecycle correction.

## Runtime validation

The isolated final sets contain 23 default, 53 strict and 53 ASan runtime
processes, all successful. Compile records are separate. Each covers stock
off/on (387/509), generational off/on, 18 worker cases, and allocation account.
Strict and ASan add constructor/fairness, hard-check, native/remote/duplicate,
scheduler, recovery/rescan/leaf/admission, and coalescing controls. ASan targets
use Clang O1 with leak checks; host tools are not ASan-instrumented. Strict and
ASan share the exact ten recorded assertion/helper defines. No default-runtime
helper configuration is inferred from those builds.

The registered worker bridge and automatic control cases pass 18 and 37
runtime components. The original registered scheduler C run aborts and its
two subsequent Lua components are unrun. Original canonical.json and logs
are immutable. Exact-source/default-configuration diagnosis captures an
external fixture READY publication reopening a worker-owned close gate. It
also captures real RESET_ALLOC that the old synthetic preparation omitted.
The original canonical failure itself had no captured internal snapshot;
later diagnosis is explicitly a different execution/ELF generation.

The accepted fixture patch is 666cadc179e77d7ba9d543007d445e6c8c7ea682e555159c8bb06f9126889247;
its output fixture is e029d5e3c1789b54f3c08cd950fb8a18a1761c21e8530809c6acce5f0849df36.
The zero-worker manual close now proves actual READY refusal and successful
completion after owned publication. The async phase never revokes READY and
keeps every physical arena and stop oracle. The separate correction owner
passes 36 matched processes, including four focused/observer runs, with real
observer proof and return-only negative
controls preserved. Its original strict control reproduces the mandatory
invalid borrowed IDLE transition gate assertion. The fresh registered C and
Lua off/on generation passes all three in 45.540186508 seconds, giving 58
registered and 187 total ROOT positive runtime processes. It lives under
canonical-scheduler-v2/ and does not overwrite prior failures.

The new m3_gc2_worker_sweep_bridge registry uses the existing automatic fixture
without edits, the four exact final worker fixtures, and Linux/x64 linker
wrapping. Its 18 cases exercise RESET/SCAN, one/two workers, both stop APIs,
late and consumed teardown requests, a held claim and cursor-only progress,
and peer/worker automatic collection. Original 30/45-second alarms and
35/50-second external bounds remain. Setup failures and superseded source
generations in owner evidence are never added to positive counts.

## Separate JIT and cost outcomes

The unchanged automatic fixture with RETENTION_JIT=1 passes 9/12 combined
cases. All three sole-main worker-zero cases return INCOMPLETE_AUTO in round
four; the exact eb8 normal baseline does likewise with byte-identical stdout.
Its separate peer control passes. These 14 outcomes include four explicit
failures and no alarms/assertions/sanitizer failures. Cleanup is successful
but is never counted as automatic completion. The failing runs reach 50
compiled allocation hard checks; the other engine-enabled runs report zero
and do not establish that same compiled scheduling path. jit-matrix/HANDOFF.md
records the exact cbc7e955 baseline archive, distinct from the cost baseline's
e7e3beb9 build. Separate read-only scheduling diagnosis observes 50 hard exits
each committing 64 SSB slots and no recovery before renewing a 50us native
lease, versus about 512KiB of allocation between hard exits. A one-shot stop
after the actual machine-code comparison proves a true lease refusal before
debugger delay. Independent final enumeration matches all 258143 retained
recovery identities, all READY/LIVE/MEMBER, while the bridge is complete.
Diagnostic timing perturbations and differing endpoints are preserved. No
runtime scheduling repair is claimed by those observations.

Seven alternating CPU-30 pairs of 100,000 escaping allocations per workload
with five samples per process yield 42 processes and 210 samples. Median
paired changes: interpreted TNEW -0.08774%, interpreted TDUP -0.29689%,
JIT-enabled TNEW +0.19627%, with overlapping ranges. All source and binary
identities remain matched. This shows no material cost change in the measured
sole-mutator/zero-worker workloads; no throughput, stock-parity or general
speedup claim follows.

## Archive and remaining scope

The archive retains the owner and original diagnosis packages, all ROOT
results, canonical failure and later success, scheduler diagnosis/correction,
JIT matrix and paired cost observations. Verified UTF-8 text is copied;
NUL/non-UTF-8/ELF/archive artifacts are represented only by exact hashes/size.
The pre-integration archive manifest/status is retained, with final extension
records identifying the later accepted generation. Source build metadata and
default/assertion/helper generations are distinguished throughout.

This change does not remove the synchronous worker/phase authority, mutable
root borrowing, the large pending-chain EOF tail, same-TG held-arena starvation,
or string-retention exclusions. The sole-main JIT scheduling failure is open.
Windows/macOS validation stays deferred until release preparation as directed.

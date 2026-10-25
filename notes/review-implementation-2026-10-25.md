# Implementing the architecture review

Starting point: `fdd7adf9`. The user requested implementation of all review
recommendations, with replacement of example-specific trace templates as the
first priority. This is the live implementation ledger; completed evidence is
recorded at each commit rather than inferred from the starting gates.

## Work streams

1. Replace whole-trace rooted read/length shapes with operation-level contracts:
   operand provenance, root slots, call effects, retry/type guards, snapshots,
   and independently checked allocated locations. Grow keys/results, stores,
   and allocations only through their corresponding runtime authorities.
2. Replace first-side scalar templates and handle numeric transitions through
   actual native continuation. Report entries, exits, and rejection reasons.
3. Replace FFI signature/ordinal/register templates with ABI-class contracts;
   retain rooted metadata, native-frame and result ownership, errno, PAC/BTI,
   and independently checked allocation. Exercise real generated-frame unwind.
4. Publish an immutable root-entry certificate bound to the exact trace body,
   incarnation, layout, and machine code. Keep dynamic lifetime/identity,
   bytecode, request, and topology checks on every entry.
5. Centralize test build/run profiles and verify runtime/fixture identity.
   Replace brittle text matching with executable negative controls where
   possible. Keep semantic and post-RA verdicts independent.
6. Measure lease admission/retries and rescan-credit contention, document
   progress and suspension contracts, and test stalled participants. Optimize
   only after measurements identify a worthwhile change.
7. Validate the combined result sequentially with complete bootstrap and
   experimental gates, stock source/import corpora on ARM64 and x86-64,
   sustained concurrency, appropriate sanitizers, and release measurements.

## Coordination and acceptance

The primary agent owns rooted operations and root-entry certification. Luna
agents own scalar side transitions, FFI contracts/unwind, and test/profile plus
progress measurements. They work in isolated snapshots and return reviewed
patches. Focused builds may run separately; full umbrella gates are serialized.

No new locks, unchecked raw heap effects, or admission by an earlier validator's
verdict. Source shapes, IR graph meaning, allocated locations, snapshots,
emitted execution, lifecycle races, and adjacent malformed inputs all remain
part of acceptance. A generic contract replaces a template only when the
additional accepted shapes execute and their unsafe neighbors still reject.

## Rooted operation contracts

The numeric read and length certificates now validate each operation by
def-use relationships and scratch-slot effects. They no longer require key 1,
one table, two reads, an exact number of TMPREF/CARG nodes, or an arbitrary
eight-call ceiling. A call consumes the currently written input roots; a
status guard must immediately follow it, and a numeric result load must use
the matching output root with its own post-helper snapshot. The independent
post-RA walk also proves distinct scratch GPRs and full-width table spill
ownership. Unrelated helper calls and raw heap operations remain rejected.

The native matrix covers argument keys, fractional numeric hash keys,
induction keys, two tables and computed keys, INT results, mixed length/read
effects, and five reads per iteration. Every case produced one native trace
and 20 exits across 20 calls, with correct post-GC reentry. Both ARM64 and
arm64e/BTI passed the complete rooted-read contract, including independent
semantic/allocated mutation fixtures, aligned table-spill negatives, original
refusal/type-change/resize/GC/STOPREQ lifecycle tests, and the registered M6
case. The compiled general IR-admission fixture also passed. Log:
`/tmp/ljmt-generic-rooted-contract.log`.

The replaced source-string checks are now covered by executable positives and
malformed-neighbor tests. The production-ordering checks remain separate.
Other review work streams are still in progress; this result does not certify
the final combined implementation.

## Immutable root publication proof

Root publication now seals the final compact body with an exact identity tuple
and a monotonically allocated serial. The body, IR/snapshot pointers and
extents, prototype, original instruction, admission kind, trace number, and
machine-code/exit geometry must all match at entry. This is not a reusable
boolean or a collision-prone checksum. Compaction rollback preserves the
allocation serial but clears the seal; a new allocation cannot reuse it.

Both independent admission passes still precede publication. The final compact
layout is checked again before the certificate is release-published. Release
entry compares its bounded tuple while retaining the TG lifetime publication,
slot/body revalidation, live bytecode, topology, retirement, request, and PAC
checks. Assertion builds additionally rescan immutable contents to detect
illegal writes. Mutable snapshot counters and exit targets remain outside the
seal and retain their existing ownership rules.

The complete root-entry contract passed on ARM64, arm64e/BTI, and an ARM64 build
without `LUA_USE_ASSERT`. Certificate corruption covers every tuple field,
missing publication, and same-address/new-allocation serial mismatch. Existing
layout negatives now also require the publication scanner to refuse sealing;
assertion entry still detects their illegal post-publication mutations. The
same run passed source-generation, retirement/request-race, emitted caller ABI,
first-side ingress, and real hot-loop checks. Log:
`/tmp/ljmt-root-certificate-contract.log`. Release performance and combined
validation remain separate work items.

## Rooted key/result classes and shared test plumbing

The operation walker now accepts guarded string inputs, string constants,
boolean keys, and boolean read results as well as numeric keys/results. GC64
constant payload words cannot masquerade as independent IR values. Strings
remain rooted through the trace or guarded stack input and the existing
runtime helper retains the exact key body before using it. Boolean results
remain guard-only values with canonical primitive snapshot references.

Thirteen active-peer workloads passed on ARM64 and arm64e/BTI, each producing
one trace and 20 exits for 20 calls; boolean flips resumed correctly. The
independent semantic and allocated mutation fixture also ran against live
multi-table, string-argument, string-constant, and boolean-result traces.
Wrong constant types, null roots, GC payload references, broken scratch
relationships, and snapshot/location corruption all refused admission. The
complete rooted-read contract, existing lifecycle races, and registered M6
case passed. Log: `/tmp/ljmt-rooted-key-types-contract.log`.

Five focused contracts now use the common build/profile helper (phi spill,
numeric spill, IR admission, rooted length, rooted reads). It binds runtime,
archive, and fixture hashes to compiler/target/macro settings and checks them
before execution. The rooted-read run above used the shared plumbing on both
targets. The isolated migration validation also passed rooted length on both
targets and PHI with `CC='clang -pipe' CFLAGS='-O0 -fno-common'`. The broader
Lua test harness profile hardening is still in progress.

## Release root-entry comparison

Compared `c441404c` with the immutable-certificate implementation in matching
non-assertion, non-test-helper ARM64 experimental builds, using
`bench/arm64-entry-cost.lua`. Five process-CPU samples per workload, median:

| Workload | Before certificate | Certificate |
| --- | ---: | ---: |
| JIT, 8 iterations x 100,000 calls | 0.041809 s | 0.023436 s |
| Interpreter control, same calls | 0.017663 s | 0.018343 s |
| JIT, 1,024 iterations x 10,000 calls | 0.011685 s | 0.009771 s |
| Interpreter control, same calls | 0.152365 s | 0.155330 s |

Both JIT workloads retained one native trace and 20 warmup exits. The local
entry-dominated workload improved about 44%; the longer loop improved about
16%. These are matching release-build measurements of these kernels on this
host, not a general performance or complete-port claim. Raw output:
`/tmp/ljmt-root-certificate-release-comparison.log`.

## Progress contracts and measurement

Test-only counters now expose scalar-read attempts/hits/refusals, physical
lease admissions/refusals/rescue retries, and rescan reservations, conflicts,
credit settlement and CAS retries. They add no production instructions or
ordering changes. The focused native contract completed 1,000 reads and a
missing-key refusal while the idle reclaimer was paused, with no table waits.
A paused INSTALLING publisher could be cancelled while an independent table
continued; every pending credit was returned. Four workers also completed
40,000 independent-table reservations and balanced settlements.

The operation progress and suspension contracts, commands and measurements are
recorded in `notes/lockless-progress-contracts-and-ci-profile-2026-10-25.md`.
The separate non-test bootstrap profile measured one million direct scalar
helper calls at 81.25 ns/read. This is C-helper timing, not JIT throughput.
The complete focused progress and release-helper scripts passed on the
combined worktree; both restored the ARM64 experimental assertion profile.

## Build-profile identity

The Lua harness now records compiler, target, compile/link flags, SDK,
deployment target, target sanitizer flags and output hashes. Linked fixtures
load that exact profile, including in a fresh harness instance; stale runtime
or archive replacement and a changed valid profile are rejected. Profile
changes force a clean build, including the non-clean API path. C++ fixtures
use an explicit compiler option while retaining the archive's target/macros.

Shell builders publish the same stamp immediately after their known build.
The bootstrap and experimental umbrella gates now use the shared build lock
and profile helper. The experimental gate restores its own exact profile
after its nested contracts before running the final linked fixtures.

`m0_build_profile_identity` passed, covering compiler/macro/target changes,
replaced outputs, stale instances, C++ selection and target sanitizer flags.
The actual registered ISNEXT C fixture and complete interpreter callback
lifecycle suite also passed, including its C++ unwind executable. The broader
bootstrap run reached the concurrent string-buffer fixture but one run timed
out at its 90-second deadline; another run had passed that fixture before
exposing the now-fixed C++ compiler selection issue. This timeout is under
investigation and the combined bootstrap gate is not yet claimed passing.

Three immediate isolated reruns of the same string-buffer program passed in
5.20, 4.63 and 4.14 seconds. That does not explain the earlier deadline failure;
the full combined gate remains required after the implementation integrations.

## Observable native execution

`bench/native-coverage.lua workload.lua [arguments...]` now reports trace
publication, recording refusal reasons, code/IR sizes, parent/link metadata,
and per-exit counts. It owns the trace and exit observer hooks; a workload that
replaces those hooks fails explicitly instead of producing misleading zero
counts. Workload failures likewise cannot produce a successful coverage report.
This is an observation run, not a timing tool or a count of native iterations.

The registered `m6_jit_native_coverage` case passed on the native ARM64
experimental assertion build. It checks an interpreter control, a real compiled
numeric loop with observed exits, hook conflicts, and workload failure. The
JIT-disabled branch was also checked separately. Log:
`/tmp/ljmt-review-native-coverage.log`.

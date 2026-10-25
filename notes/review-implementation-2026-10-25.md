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

The scalar side runtime checkpoint removes selected exit-number, prototype
length, snapshot-PC and inherited-register templates. A selected parent exit
is copied under SMR and exact recorder-token ownership; inherited values are
validated by slot, IR provenance, type and actual parent location. Children
can close naturally as scalar loops or return through the certified parent
tail. The independent allocator pass checks emitted layout, snapshots and
loop geometry. Child publication seals both allocation generations and their
exact body/code/topology tuple; retirement detaches the authenticated incoming
edge before unlinking the child. Heap effects remain excluded from children.

The initial integrated fixture passed on ARM64 and arm64e/BTI, but its entry
counter was insufficient: it counted lookups, not successful native handoffs.
The zero-exit result in `/tmp/ljmt-side-integrated-adaptation2.log` did not prove
execution. Release measurements exposed the gap. The published-child validator
was missing the emitted loop offset, so it could refuse an otherwise published
child. Entry now supplies that offset and derives the semantic extent from the
actual trailing RENAME markers. A new counter increments only at the final
successful native handoff. The fixture requires positive successful entries,
zero cleanup refusals, a patched parent edge, and bounded native trace exits;
normal loop completion legitimately exits native code. The stronger private
fixture passes on both targets. Combined integration validation follows below.

The integrated scalar capability contract passed, along with complete FORL,
FUNCF and rooted-store contracts on both targets. The merge initially retained
an old LOOP-only allocator check that rejected FORL; it was removed before this
checkpoint. Logs: `/tmp/ljmt-side-merge-scalar.log`,
`/tmp/ljmt-side-merge-{forl,funcf,store}3.log`. Copied-child corruption cases,
remaining shell-contract migration and the full combined gates are subsequent
acceptance work, not implied by these focused passes.

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

The side integration exposed an observer limitation: transactional ARM64 child
publication requires an absent TRACE callback. The default coverage tool's
trace observer therefore changes side-recording eligibility. Its output now
labels that observer, and `--exits-only` installs only the exit callback so
adaptation can proceed. That mode reports exit totals and per-trace-number
counts; publication, recording refusals and flush generations are unobserved,
and reused trace numbers are intentionally not described as one incarnation.
Interpreter and generated-loop controls, hook ownership and failure reporting
passed in both modes (`/tmp/ljmt-native-coverage-exits-only.log`). Direct child
execution remains a separate native fixture assertion.

## Rooted scalar store effects

Existing-key numeric and boolean stores now use generic rooted helpers on the
experimental ARM64 path. Table and key inputs are retained under exact leases,
revalidated, and handed to enumerated TG anchors before invoking the existing
cooperative keyed-store protocol. Temporary leases and SMR readers are closed
before retry/wait boundaries. String writes invalidate the negative metamethod
cache; a missing/deleted key resumes ordinary Lua assignment. This retains the
existing resize/weak/barrier rules and does not expose raw array/hash stores.

The helper is a cooperative operation, not a no-wait try. It may reserve anchor
storage and retry during resize; a refusal following a raced CAS does not mean
no write ever occurred. This matches the existing store protocol. Semantic and
post-RA walkers independently validate roots, the scalar argument chain, status
guard, allocated scratch registers, and post-store guard snapshots. They impose
no trace-wide store count, key, table count or reference ordinal.

`tools/ci/arm64_jit_rooted_store_contract.sh` passed on ARM64 and arm64e/BTI:
six live active-peer loop shapes each had one native trace and 20 exits across
20 calls, including mixed reads/stores and computed keys/values. Deleted-key
metamethod fallback was checked. Direct helper tests covered source handoff,
full GC, stack growth, resize, first/second anchor reservation failure, missing
and invalid keys, closed reclamation admission, OS error preservation and
protected error rollback. Both independent validators rejected corrupted
argument/root/status/snapshot graphs. Log:
`/tmp/ljmt-review-rooted-store-contract.log`.

The new M6 case is registered separately. GC-valued stores, new-key trace
construction and allocation effects still require their own operation proofs;
this checkpoint does not claim those paths.

## Function-entry publication stage

The immutable root scanner now distinguishes private FUNCF publication from
live JFUNCF entry. Before bytecode patching it verifies the original header and
the ordinary independent post-RA proof; runtime entry still verifies the exact
patched generation. This fixes the publication assertion without duplicating
the bytecode grammar or relaxing the live-entry checks. The focused FUNCF
record/lifecycle contract passed on ARM64 and arm64e/BTI, including native entry,
GC and retirement. Log: `/tmp/ljmt-review-funcf-prepub.log`. Broader FUNCF
operation admission is a separate change under implementation.

## Integer FORL operation graphs

Integer FORL bodies now use the generic scalar/effect walkers. The loop control
proof follows the hidden index, stop and step slots, recurrence, direction and
overflow guards by their def-use relationships; it no longer requires the
example accumulator, two PHIs, or adjacent IR ordinals. The independent
allocated proof verifies the same authorities from immutable IR and concrete
locations. Unchecked integer ADD remains restricted to the two proven control
increments; an arbitrary integer body operation must retain its overflow guard.
CALLXS effects retain their separate ABI validator.

Five separately flushed kernels each published and executed one native trace
with 20 exits across 20 calls, on both ARM64 and arm64e/BTI: one accumulator,
three accumulators, positive and negative dynamic steps, and a floating-point
body with integer induction. The driver is interpreted and each case checks
its own function identity, code and linkage, plus post-GC reentry. The existing
native JFORL lifecycle tests also passed. Both independent mutation suites
passed, including out-of-range step references and unguarded body additions.
The complete rooted-read contract and registered ARM64 scalar/pointer CALLXS
lifecycle suite passed after integration. Logs:
`/tmp/ljmt-review-forl-{native,ir,reads,ffi}.log`.

## Table-construction effects

Unsunk TNEW is now a scalar-walker effect, including table PHIs and full-width
snapshot/spill ownership. It uses the existing `lj_tab_new0_forjit` and
`lj_tab_new1` constructors: complete array/hash initialization, enumerated
construction anchor, READY publication, ownership linking, and deferred
accounting remain unchanged. The native `jit_base` lease prevents collection
from finishing while values live in machine registers. Existing allocation
checks and XPOLL restore a certified snapshot before interpreter-owned GC
progress; no new helper, lock or raw heap store was added.

The focused contract passes on ARM64 and arm64e/BTI. Empty, two-live-table and
FORL kernels each have one native trace and 20 measured exits over 20 calls.
A constructor hook observes thousands of actual native allocations and checks
READY, anchors and all initialized storage. Capacity hints 0, 4, and 65 array
slots plus 16 hash nodes exercise both native constructor calls; the largest
case has 23 measured exits because allocation pressure also requests GC exits.
Each workload survives eight concurrent peer collection cycles, retains fresh
and distinct returned tables, and reenters after collection. Independent
semantic/post-RA mutations reject malformed capacities/types/PHIs, raw stores,
sunk locations, wrong register class and partial-width spills. The general IR
contract and registered M6 allocation/store cases also pass. Logs:
`/tmp/ljmt-review-tnew-contract-final.log`, `/tmp/ljmt-review-tnew-ir.log`, and
`/tmp/ljmt-review-allocation-registered.log`.

Sunk allocation, TDUP, raw initialization stores, new-key stores and GC-valued
stores retain their separate admission boundaries. This checkpoint proves
construction through the existing runtime publication protocol.

## Combined bootstrap checkpoint

The complete bootstrap gate passed on frozen `450471dc`: assertion interpreter,
GC2 hard assist, stock cases, TG hooks, coroutine/dead-resume and FFI callback
lifecycle tests, followed by restoration of its exact bootstrap profile. The
concurrent string-buffer fixture also completed in this run. This supplies a
passing combined run; it does not explain the earlier intermittent timeout.
Log: `/tmp/ljmt-review-bootstrap-450471dc.log`.

The original entry benchmark used `hotexit=1000`; its large exit count does not
prove adaptation failure with ordinary settings. The historical review now
states that limitation. Numeric-side acceptance must identify a genuine child
and the patched parent exit, rather than infer adaptation from trace count.

## Nested and sanitizer shell profiles

The combined experimental run exposed a restoration mismatch: the migrated
PHI contract restored its standalone profile, dropping trace helpers required
by the next scalar fixture. The umbrella now passes its complete restoration
profile to nested shared contracts; PHI and numeric-spill contracts respect it.
The actual PHI-then-scalar sequence passes, including native scalar/FMA exits,
independent corruption checks, GC and flush. Log:
`/tmp/ljmt-review-nested-profile.log`.

The shell profile helper also propagates target compile/link/shared-link flags
and debug flags explicitly through builds, restoration, stamps and fixture
compilation. Previously target-only sanitizer flags reached the runtime through
the environment but were absent from the fixture compiler command. With the
fix, the complete allocation contract passes under AddressSanitizer on both
ARM64 and ARM64e, including all capacity variants, independent admission
mutations and concurrent GC. Runtime sources are frozen at `a836be4b` (same
production code as `450471dc`) in an isolated copy. Flags were `-O1 -g
-fno-omit-frame-pointer -fsanitize=address`, matching link/shared-link sanitizer
flags, and `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=1`.
Leak detection is separate from this retained-arena test. Log:
`/tmp/ljmt-review-tnew-asan.log`.

## Sanitizer expansion and gate maintenance

At the same frozen production checkpoint, AddressSanitizer also passed the
complete rooted-store contract on ARM64/ARM64e, both 509-case stock source and
original-bytecode corpora, 300 join/GC cycles, and six-thread cdata stress with
24,000 iterations per worker. Logs: `/tmp/ljmt-review-rooted-store-asan.log`,
`/tmp/ljmt-review-stock-asan-source.log`,
`/tmp/ljmt-review-stock-asan-import.log`, `/tmp/ljmt-review-join-gc-asan.log`,
and `/tmp/ljmt-review-cdata-asan.log`.

The registered `m4_tsan_drivers` passed with target-only ThreadSanitizer flags
on frozen `92393d92`. These are the real C thread/TG TLS/sleep and bounded MPMC
channel drivers, not a claim of complete VM/JIT race freedom. Log:
`/tmp/ljmt-review-tsan-drivers-target.log`. Host generators must remain
uninstrumented when only the target linker receives sanitizer flags.

The next experimental gate stopped at an obsolete source assertion in the
side-assembler contract. Root publication now has separate geometry and
immutable-certificate fail-stops; the contract still demanded exactly one.
It now checks both guards and the expected two fail-stops. The full focused
contract passes on ARM64/ARM64e, including private compaction, reset, MCODELM
recapture and the raw ARM64e negative publication seal. Log:
`/tmp/ljmt-review-side-asm-fixed.log`. The whole experimental gate remains
pending the combined generic FUNCF/FFI/side implementation.

## Exact function-body generation

FUNCF traces now reserve a bounded copy of their bytecode body in the compact
trace allocation. The certificate binds its pointer/count and entry compares
acquire-loaded live words with the copy; the separately patched JFUNCF header
retains its existing exact generation checks. This distinguishes two legal
bodies (for example, `return true` and `return false`) without hashes or release
IR/snapshot scans. Other trace kinds allocate no bytecode tail and incur no
additional body scan. Private compaction/reset and exact-size reclamation
preserve the allocation count.

The full existing FUNCF contract passes on ARM64 and ARM64e, in both assertion
and release configurations. New independent corruption checks cover allocation
count, certificate count/pointer and copied word; the existing paused
POSTMETADATA header/body/return mutations still refuse entry. Logs:
`/tmp/ljmt-review-funcf-generation.log` and
`/tmp/ljmt-review-funcf-generation-release.log`. General scalar FUNCF bytecode
and IR admission are a separate patch currently under review.

## Common GC object alignment

UBSan found that `GCobj` inherited `GCtab`'s 16-byte alignment, while ordinary
objects (including strings) can occupy valid 8-byte-aligned addresses. Even
forming a union member address then asserted an alignment the allocation did
not have. The common union view is now capped at 8-byte alignment; concrete
object types, table layout and the 16-byte atomic pair are unchanged.

A standalone UBSan regression exercises an address that is 8 mod 16 and proves
the generic reference/tag/header paths, while separately asserting table-pair
alignment. It passed on native ARM64 and an ad-hoc-signed x86-64/Rosetta binary.
Both ARM64 umbrella gates now run it. The table-pair concurrency and emitted
CASPAL-only contract also passed. With the fix, the entire TNEW contract passes
on ARM64/ARM64e under `-fsanitize=undefined` without disabling alignment checks,
including native construction, independent mutation tests and concurrent GC.
Logs: `/tmp/ljmt-review-aligned-tab-pair.log` and
`/tmp/ljmt-review-tnew-ubsan-aligned.log`. The sanitizer runtime used an isolated
`92393d92` source copy plus this alignment fix; this does not yet validate the
later generic function, side or FFI patches.

## Generic FORL gate and x86-64 regression checkpoint

The next full experimental run reached FORL but stopped at source-text checks
for exactly two ADDs and fixed control-instruction adjacency. The generic
control graph intentionally removed those restrictions. The wrapper now relies
on its executable semantic/post-RA mutation contract, while retaining separate
publication/VM ordering checks. The nested contract receives the wrapper's
exact build flags. The complete focused FORL contract passes on ARM64/ARM64e,
including dynamic positive/negative steps and native versus branch-only entry.
Log: `/tmp/ljmt-review-forl-generic-contract.log`.

Frozen `cc7b6899` also passes both 509-case stock source/original-bytecode corpora
on x86-64/Rosetta, the registered errno case, nested-universe table guard and
foreign-thread hotcount generation fixtures. The progress fixture initially
assumed dual-number representation when inspecting its preserved output; it
now uses the representation-independent numeric accessor. With that fixture
fix, x86-64 completes 1,000 reads plus a missing-key refusal during a paused
reclaimer (zero waits), independent-table progress during a paused publisher,
and 40,000 balanced rescan credits across four workers. No runtime change was
needed. Executables were ad-hoc signed and fixture/archive profiles matched.
Logs: `/tmp/ljmt-review-x64-stock-{source,import}.log`,
`/tmp/ljmt-review-x64-errno.log`, and
`/tmp/ljmt-review-x64-t-{gc2-table-store-guard,jit-hotcount-generation,lockless-progress-metrics}.log`.

## Function/loop profile consolidation and compiler selection

FUNCF and FORL now use the common profile helper for builds, compilation,
execution deadlines and restoration. Their native, independent mutation,
randomized-code-placement and ARM64e/BTI checks remain. This removes 214 net
lines of duplicated shell plumbing. Both complete contracts passed in the
isolated migration copy at `1fc79b0a` (integrated as `98cfd4fb`):
`/tmp/ljmt-profile-funcf-final.log` and
`/tmp/ljmt-profile-forl-final.log`.

The migrated FUNCF contract also passed with target-only AddressSanitizer on
both targets, including randomized MCODE and profile restoration. `CFLAGS`
and `LDFLAGS` were empty; instrumentation was supplied through
`TARGET_CFLAGS='-O1 -g -fsanitize=address -fno-omit-frame-pointer'`,
`TARGET_LDFLAGS` and `TARGET_SHLDFLAGS`. Host generator flags stayed separate.
Leak detection was disabled. Log: `/tmp/ljmt-funcf-target-asan.log`.

The full experimental run at `b59a91de` reached the native-loop contract after
passing the generic FORL/FUNCF and earlier contracts. That wrapper selected
Homebrew Clang from PATH for the fixture while the runtime used Apple's
compiler. The Homebrew pointer-authentication header lacks Apple's
`ptrauth_nop_cast`, so the ARM64e fixture could not compile. The common helper
now exports its selected `CC` to nested wrappers. The native-loop wrapper
also selects the SDK compiler by default and passes that compiler explicitly
to its runtime builds, including restoration. Its complete standalone run,
with both `CC` and `SDKROOT` initially unset, now passes on ARM64/ARM64e:
`/tmp/ljmt-review-native-loop-sdk.log`. No runtime pointer-authentication
semantics were changed.

The complete experimental gate then passed on frozen `12e1011b`, including
the generic FUNCF/FORL contracts, numerical variants, rooted operations,
native lifecycle and registered FFI/VM cases. Log:
`/tmp/ljmt-review-experimental-12e1011b.log`. This checkpoint precedes the
generic FFI and side implementations currently being reviewed in private
copies; their combined validation is still required.

## Actual generated C++ callback unwind

The C++ fixture now installs a plain native observer through `dlsym`. Inside
the bridge, before invoking the erroring Lua callback, it verifies an ACTIVE,
synchronized generated native frame, the exact foreign function, its trace
pin, TG VM state and JIT base. The callback error is triggered only after
those checks; an interpreted first loop iteration cannot satisfy this proof.
After `pcall`, the fixture verifies the C++ catch/rethrow and destructor
counts, empty native frames and XSAVE roots, and zero trace pins. It then
executes successfully after GC and a full trace flush, requiring native calls
again. This closes the former gap where an existing trace and total call
count could coexist with an entirely interpreted failing call.

Three runs per target passed on ARM64 and ARM64e/BTI using frozen `12e1011b`
plus the fixture and C++ header fix: 1,195 observed generated calls per run,
one error from a verified generated call, and successful recovery. Logs:
`/tmp/ljmt-cpp-observer-final.log` (ARM64; also records the initial ARM64e
compile diagnostic) and `/tmp/ljmt-cpp-observer-arm64e-final.log` (all ARM64e
runs passing). The header fix gives the loaded exit-slot address its existing
`MCode *` type before the no-op pointer-authentication cast; this preserves
bits and avoids dereferencing `void *` inside Apple's macro in C++.

The private generic-FFI M7 run also passes with this fixture:
`/tmp/ffi-m7-final-cpparith.log`. Its suite registration will be integrated
with that implementation. These tests establish this generated unwind and
recovery path; they do not establish every foreign exception/aggregate ABI.

## Generic FFI call effects

The ARM64 FFI path now classifies fixed scalar signatures from stable CType
snapshots and checks each call's operands, type guard and native-frame protocol.
It accepts multiple calls in one numeric loop and no longer matches three
complete example IR/register/snapshot templates. The independent allocated
walk checks argument/result carriers, caller-clobbered live values, spill
extent, snapshot ownership and the enter/CALLXS/leave sequence. Pointer results
must own the matching rooted CNEW box and payload store before leaving the
native frame. Call snapshots are associated with their matching XSAVE/leave
region rather than one global continuation PC.

The classifier models separate scalar GPR/FPR argument banks, including narrow
integers, enums and pointers. Classification alone does not admit every possible
IR graph or return representation. Outgoing stack arguments, aggregates/sret,
variadics, vectors and complex values remain closed; float and boxed 64-bit
result graphs still need their own emitted-layout evidence. Apple's platform
ABI reference is [Writing ARM64 code for Apple platforms](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms).

The focused ABI matrix passed on ARM64 and arm64e/BTI. Positives include six
integer arguments, mixed integer/double arguments, three foreign calls per
loop and unused results. Negative controls include unsupported ABI classes and
a valid but wrong callsite continuation. The integrated M7 case also passed
the original scalar/pointer admission mutations, generated calls, remote flush,
integer/double/pointer lifecycle tests and the C++ callback unwind fixture
(1,195 generated calls). Log: `/tmp/ljmt-ffi-integrated-m7.log`.

The stronger scalar-side fixture passed against the same combined production
code using the shared profile runner, strict fixture warnings, archive/object
identity checks and a deadline. Log:
`/tmp/ljmt-side-ffi-integrated-native.log`. Full combined gates and the final
caller-clobber audit are still pending at this checkpoint.

## Accepted side entry and release measurements

The combined root-entry contract passes on ARM64, arm64e/BTI and release at
`47ba8734`, including certificate corruption, mutable-state/request races,
emitted VM entry ordering and the stronger scalar-side fixture. Release runs
now exercise side adaptation explicitly. Log:
`/tmp/ljmt-root-entry-combined-47ba.log`.

Release child entry uses the generation-bound publication seal for immutable
IR and parent binding layout, retaining live topology, body/lifetime, selected
snapshot, exact edge, bytecode and PAC checks at both acquisitions. Assertion
builds continue the full independent post-RA integrity walk. On this Mac, the
same release profile (no assertion/test-helper instrumentation) measured:

| Kernel, calls | Full child scan | Certified child entry | Interpreter |
| --- | ---: | ---: | ---: |
| 1,024 iterations, 10,000 calls | 0.026515 s | 0.014204 s | 0.156129 s |
| 8 iterations, 100,000 calls | 0.192821 s | 0.068404 s | 0.019754 s |

These are medians of five process-CPU samples from
`bench/arm64-entry-cost.lua`, initial accumulator `0`, `hotexit=2`. Both JIT
profiles published a root and child and observed 41 native exits over 20
warmup calls. Timing removes the observer. The before snapshot is private
`24324076` (combined production before `ecc63dd0`, with the development print
removed); the after snapshot contains `47ba8734`. The runtime changes between
them are the side seal entry path and its exact selected-exit identity check.
Before log: `/tmp/ljmt-side-release-accepted-before.log`; after build/profile:
`/tmp/ljmt-side-release-accepted-after-build.log`. The after timing results are
recorded in this table from the command output. These local samples do not
establish general workload speedups; short-loop native entry remains costly.

The first-side lifecycle fixture now derives the selected exit from the actual
child IR_BASE, obtains its code from `trace_mcode(child)`, and requires that
exact parent edge before entering native code. Previously a token-interrupted
attempt's stale exit number could make the test inspect a different fallback
slot. With this corrected identity, default repeated ROOT-tail GC-claim,
scoped-flush and full-flush cases pass on ARM64 and arm64e. A separate LOOP
child lifecycle extension and the final combined experimental gate follow.

## Combined bootstrap and explicit child retirement

The complete bootstrap gate passed at `2e765c3e` with all default branches,
including stock tests, GC2 hard assist, C API roots, errno/unwind landings,
coroutines, threaded callbacks and arm64e callback lifecycle. It restored the
requested assertion/bootstrap profile. Log:
`/tmp/ljmt-review-bootstrap-combined-f849.log` (the filename identifies its
production parent; `2e765c3e` added notes only).

The extended first-side contract now executes four ROOT-tail children and one
self-linked LOOP child and requires the expected topology explicitly. Each
case verifies the actual selected edge, successful native entry and bounded
child exits before GC-claim, scoped-flush and full-flush retirement. Default
repetitions pass on ARM64 and arm64e with matching fixture/archive profiles.
Log: `/tmp/ljmt-first-side-default.log`; integrated changes `0643f8d4` and
`33b0a8a7`. No production retirement change was needed.

The contemporaneous x86-64 stock, errno, atomic, progress and direct-C shared
runtime checks are recorded in
[`x64-rosetta-final-regression-47ba-2026-10-25.md`](x64-rosetta-final-regression-47ba-2026-10-25.md).

## Remaining profile consolidation and umbrella coverage

The first-side publication seam now uses the common profile helper for its
builds, archive/fixture identity, locking, deadlines and restoration. Its
existing publication ordering and negative configuration checks remain.
Default two-repetition GC-claim, full-flush and scoped-flush cases passed on
ARM64 and arm64e in the isolated migration copy, including restoration with
the seam disabled. Log: `/tmp/ljmt-first-side-publish-profile-8aed.log`.

The experimental umbrella now explicitly includes the registered generic FFI
M7 suite on ARM64 and arm64e/BTI, with private library/fixture temporary paths,
and runs the native-coverage observer contract. It restores its own exact
profile after the FFI suites. Previously its final registered runtime case
was M5, so a passing umbrella alone did not include this M7 evidence. Shell
syntax and diff checks pass; the full updated gate is pending the final
CALLXS snapshot-anchor integration.

The first combined preflight at `3a4b961e` passed interpreter GC2 checks, then
the strict emitter compile caught an unused snapshot parameter in the new
generic FFI helper. The redundant parameter is removed; the complete emitter
contract now passes (`/tmp/ljmt-emitter-generic-werror.log`). This preflight is
not a complete experimental pass. The publication-profile migration was also
rerun with the private working directory and source SHA printed in the log:
`/tmp/ljmt-first-side-publish-profile-explicit.log`, source `d96f128d`.

## Resize and retirement contention measurements

The progress fixture now forces an array generation change while a scalar
reader holds its leases, checks output-preserving refusal, then successful
retry. A separate attached reader measures successful and refused try calls
during bounded resize churn. Output starts with a sentinel, and latency arrays
are sorted before reporting independent success/refusal percentiles. Caller
retry counts are separate from GC2 lease retries. The fixture reports exhausted
16-attempt budgets rather than inventing a bounded eventual-success guarantee.

Final explicit-directory runs passed on ARM64 and arm64e/BTI. The ARM64 sample
completed 4,000 logical reads with 3,050 caller retries; arm64e completed 3,999
and reported one exhausted budget, with 2,962 caller retries. Both forced
generation-change cases refused once and then succeeded. These are
instrumented helper samples with deferred retired arrays, not full collector
tail latency or release-speed claims. Commands, profiles, raw percentiles and
limitations are in
[`lockless-progress-contracts-and-ci-profile-2026-10-25.md`](lockless-progress-contracts-and-ci-profile-2026-10-25.md).

## FFI source authority and independent snapshot regressions

CALLXS XSAVE markers now carry the recorded source continuation as two bounded
literal halves. Both validators bind completed-call and allocation snapshots
to that authority; a different valid CALL in the same prototype is insufficient.
An isolated allocation-frame constant mutation was accepted by the old gate
and is rejected by both fixed walkers. Folding emits ordered XSAVE markers
before inspecting literal operands. Allocator suffix checks exclude self,
forward, metadata and constant-payload RENAME sources and conflicting registers.

The complete native ARM64 M7 passed on frozen private `20e326cd` before
integration, including the large-source native-call witness, mixed ABI banks,
multiple calls, pointer roots, errno, callbacks, C++ unwind and remote GC/flush.
Log: `/tmp/ljmt-anchor-integrated-m7-20e326cd.log`. The stronger witness found a
double-subtracted register mask; using the already filtered `RSET_GPR` admits
the valid X19 allocation while reserved-register negatives still reject.
Details and the corrected history of early private validation are in
[`arm64-callxs-source-identity.md`](arm64-callxs-source-identity.md).

The shared XSAVE literal/fold change also passed both 509-case x86-64/Rosetta
stock corpora, errno and generated foreign-call/callback-stack tests; see
[`x64-rosetta-xsaveliteral-final-regression-815e7d5e-2026-10-25.md`](x64-rosetta-xsaveliteral-final-regression-815e7d5e-2026-10-25.md).
The subsequent source-identity and register-mask changes are ARM64-only.
Final combined gates and additional semantic caller-footer negatives remain
pending; this focused pass does not replace them.

Shared C/C++ helper libraries now use the same compiler wrapper as linked
fixtures, including target architecture, CPP flags, target sanitizer/linker
flags, SDK and deployment target. The old helper bypassed that wrapper and
produced arm64 C libraries for arm64e tests. The profile unit contract covers
pre-build shared-library flags, and the complete M7 now passes on arm64e/BTI
with matching libraries (`/tmp/ljmt-profile-m7-arm64e.log`). The M7 runner also
accepts an explicit `LJ_ARM64_CALLXS_ASSERT=0` runtime profile; the umbrella
requires both assertion and release-entry behavior on both targets. These
release tests retain fixture helper instrumentation and are correctness checks,
not release-performance measurements.

## First resumed instruction uses the recorder overlay

The final combined gate at `9b0c6e18` stopped in the rooted-length contract:
forcing the generation-retry exit with `hotexit=1` triggered `rec_check_slots`
(slot 7 was a number in the interpreter and TAB in the recorder). A standalone
reproduction localized this to the first resumed LEN, after warmup and both
admission mutation passes succeeded. The ARM64 exit landing still used
`GL + GG_G2DISP` for ordinary/function bytecode, so it skipped the owner's
newly installed recorder overlay for one instruction. The following ADD
reached the recorder with a stale inherited table value.

The landing now loads its dynamic target and dispatch-slot pointer from the
current TG's `DISPATCH`, matching ordinary VM dispatch and the x64 exit path.
No lock, assertion relaxation, or admission exception is added. The existing
generation-retry fixture is the regression: the unchanged test failed at
`9b0c6e18` and passes with this correction. Its full contract passed on ARM64
and arm64e/BTI, including native helper execution, independent certificates,
STOPREQ unwind/resume and full-GC reentry. Log:
`/tmp/ljmt-rooted-len-exit-overlay-contract.log`. The complete experimental
gate must be rerun with this fix; the failed combined log remains
`/tmp/ljmt-review-experimental-final-9b0c6e18.log`.

The strengthened regression installs only a RECORD observer after warmup and
requires a LEN event for the exact traced function during the forced retry.
It does not require a heap-effect child to publish. This observer version also
passed the complete focused contract on both targets, log
`/tmp/ljmt-rooted-len-record-observer.log`.

A release-runtime negative control also confirmed the observer: the old VM
exit source from `9b0c6e18` fails `retry_record_len_calls > 0`, while the fixed
source passes. The runtime omitted `LUA_USE_ASSERT`; the separately compiled
fixture retained assertions. Logs: `/tmp/ljmt-record-observer-old-run.log` and
`/tmp/ljmt-record-observer-fixed-run.log`. The isolated base was `3621183f`
with the observer fixture subsequently committed in `241a41e8`.

## GDB metadata coverage follows the generalized side boundary

The combined rerun at `241a41e8` passed rooted effects, native scalar side
adaptation, and first-child publication/retirement, then exposed a stale GDB
fixture: its "unsupported" example was a scalar conditional increment now
accepted by the generalized validator. That example is retained as a positive
which requires a real child, its exact parent/selected edge, distinct GDB
metadata, and successful ready-before-registration accounting. The selected
exit is derived from the child's IR_BASE rather than a fixed ordinal.

The negative now takes a conditional table-store path outside scalar-child
admission. It verifies the store executes in the interpreter and still
requires zero child publication and zero metadata preparation/registration.
No production gate was weakened. The complete GDB-JIT contract passed with
helper and production profiles on ARM64/arm64e, including rollback and delayed
retirement. Log: `/tmp/ljmt-gdbjit-generic-side-contract.log`. The stopped
combined log is `/tmp/ljmt-review-experimental-final-241a41e8.log`.

## Exit-table accounting isolates the root retry

The later exit-contract preflight at `2e7b4e1d` reached all root ownership and
native XPOLL/terminal checks, then failed its exact allocation count at close.
A diagnostic build observed three allocations and two frees before close,
and three allocations and three frees after close. With `hotexit=1`, a rejected
side recording allocated and freed an additional provisional table; no child
was published. There was no outstanding allocation at shutdown.

The root-slot fixture now uses `hotexit=1000` to isolate its forced assembler
retry and published root. It retains the exact two allocations/two frees,
snapshot and machine-code checks, production retargeting, and actual SIGTRAP
and ARM64e authentication-failure cases. Generic side publication and retirement
remain covered by the separate first-child contracts. The original stopped
log is `/tmp/ljmt-review-exit-final-2e7b4e1d.log`; diagnostic counts are in
`/tmp/ljmt-exittab-diag-2e7b4e1d/result.log`. The complete exit contract now
passes on ARM64 and ARM64e/BTI, including trace retirement, immutable gate
disassembly, reset and all signal-based target checks. Rerun log:
`/tmp/ljmt-exit-isolated-root-2e7b4e1d.log`.

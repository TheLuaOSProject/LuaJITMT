# Lockless completion plan

Reviewed: 2026-09-04, starting at `a649f737`.
Updated: 2026-09-05 after worker SWEEP scheduling and native teardown validation.

This is the operative continuation of the original plan. It preserves the
requested end state: one shared Lua heap, safe ordinary racy Lua programs,
fully nonblocking GC/JIT/FFI runtime protocols, and performance near or above
stock LuaJIT on x86-64 Linux, macOS, and Windows. It supersedes conflicting
sketches, platform restrictions, and dated implementation-status paragraphs in
documents 00–14. A beta release, a dormant primitive, or a passing narrow test
does not establish completion.

User-directed sequencing (2026-09-04): implementation and testing focus on
Linux until the next release is ready. Windows and macOS investigation/fixes
resume before that release. Their known gaps stay recorded below without
blocking independent Linux progress or narrowing final scope.

User priority (reaffirmed 2026-09-05): stability comes first. Prioritize
reproducible races, lost GC edges, lifetime/reclamation errors, semantic
regressions, and stalled progress. Performance remains required, with changes
backed by those safety checks and measured cost. The work areas below describe
dependencies; they do not put a performance rewrite ahead of a demonstrated
correctness defect.

## Evidence and design rules

1. An ordinary operation must not need a particular suspended peer to resume.
   A yield, futex, or timeout inside a retry loop does not make that dependency
   lock-free. A bounded helper returning RETRY is useful only when its public
   caller can complete through helping, a valid alternative, or speculative JIT
   abandonment followed by ordinary execution.
2. `ffi.cdef` writers may serialize. Operations whose public contract selects
   blocking, such as join, channel waits, user mutexes, sleep, and external C/IO,
   may block the caller. Internal ownership held by such a caller must not
   prevent unrelated VM/GC/FFI operations. Historical implementation exceptions
   for other control surfaces are open work, not new exclusions from the goal.
3. Safety includes type/tag integrity, preserved completed writes, exact roots,
   weak semantics, finalizer ordering and run-once behavior, error unwinding,
   coroutine handoff, and native callback reentry. Replacing an unresolved
   value with nil, dropping work, permanently disabling collection, or silently
   narrowing admitted Lua programs is not an acceptable progress mechanism.
4. Allocation, preparation, and speculative computation happen before an
   irreversible publication wherever possible. Published transactions retain
   all data and cleanup authority needed by a different helper. No transition
   may leave its only payload, completion certificate, or decrement authority
   in the initiator's C locals.
5. CAS retries require a concrete progress argument. Check-to-CAS windows,
   same-address reuse, version wrap, nil/value ABA, cancelled attempts, and
   delayed helpers must be modeled explicitly. Do not infer correctness from
   a successful stress run or from changing plain accesses to atomics.
6. Defer physical reclamation when proof is missing; then implement and test
   eventual reclamation. A system that never frees a class of ordinary garbage
   is incomplete. Measure retained bytes and reader/transaction lifetimes,
   including sustained MT and parked GC workers.
7. Keep the normal single-thread path cheap. Measure pre-activation,
   sticky-MT with one remaining mutator, and active MT separately. A dormant
   feature's zero hot-path cost says nothing about its production cost.

## Source-based review

Detailed findings and their current-source entry points are in:

- `notes/runtime-table-review-2026-09-04.md`
- `notes/runtime-gc-review-2026-09-04.md`
- `notes/runtime-jit-ffi-review-2026-09-04.md`
- `notes/runtime-performance-review-2026-09-04.md`

The useful foundations include per-TG execution state, atomic TValue
publication, immutable executable code with writable exit targets, native
trace-frame certification, exact GC object/reader admission, rooted bounded
table reads, helpable table rescan tokens, and detached FLUSH events. Retain
them unless new correctness or performance evidence contradicts their design.

The review does not establish an exhaustive absence of additional races. These
are verified reasons the full goal is still open:

| Area | Finding at review entry | Required direction |
| --- | --- | --- |
| Table resize | Structural owner plus source `FORWARD` can strand the only value in an owner's local variable; descriptor migration is dormant | Durable exact payloads, helpable migration/publication, GC and native grace |
| Table iteration | Ordinary scalar ITERN waits for global SMR while an unrelated IDLE reclaimer is suspended | Prove independently leased iteration progress, including exact source validation and terminal END; preserve general hash/GC-result lifetime requirements |
| Descriptor installation | Separate capacity-shadow store can corrupt a winning descriptor before the losing ownership CAS | Publish control and capacity in one atomic pair; deterministic competing-generation test |
| Automatic GC | Safe boundaries consume durable requests and workers now schedule missing SWEEP preparation; a sole-main JIT workload still exceeds its automatic completion bound | Resolve compiled-allocation assistance, same-TG arena fairness and unbounded EOF work, then complete asynchronous phase ownership; retain independent finalizer suppression |
| Closure construction | Test-injected nested collection now defers an unfinished owner, and scalar continuation preserves sweep turns for other TGs | Complete same-TG arena fairness and asynchronous phase ownership; retain exact construction state and eventual publish/cancel completion |
| Native acknowledgement | Completed exact owner-root actions can release through the unique remote executor; local/duplicate and broader actions still hold | Replace mutable-root borrowing and retain exact completion authority; local native-depth polls can overlap a remote pre-claim scan |
| First MT attachment | Mode-0 traces omitted the TG request poll and real attachment waited for natural exit | Every XPOLL now observes the TG request; the larger attachment/flush ownership dependencies still require asynchronous completion |
| Worker scheduling | MARK-close ownership loss can be reported as progress and cause repeated drain-loop execution | Return/defer without false progress or peer sleep; preserve durable retry |
| GC work per mutation | Public SWEEP barriers can repeatedly queue an entire growing table; traversal charges a whole vector as one work unit | Prove redundant barrier elision, bound traversal by slots/bytes, and measure full-suite phase/history amplification |
| Table scan authority exhaustion | A long-lived table's 32-bit dirty counter saturated into a permanent universe-wide reclamation veto | Persistent wide promotion now preserves collection through that rollover; retain full-namespace containment and current cycle-namespace limits |
| Strings | Physical body reclamation requires sole-main explicit collection without GC workers; peer/worker cases retain 24,576 dropped bodies through 12 completed cycles | Concurrent canonicalization, unlink, and eventual body reclamation; preserve existing exclusion until replacement lifetime proof is complete |
| String interning | Header resize claim waits for pinned readers and blocks entrants | Immutable successor topology and helpable publication |
| Marking/JIT overlap | One worker token serializes tracing; each mark quantum excludes every active `jit_base` | Parallel mark ownership plus certified concurrent native/JIT roots |
| CType state | General FFI readers wait for the parser sequence, including existing user-defined types | Private parser transactions with immutable committed versions |
| Foreign callbacks | Same callback slot shares one hidden carrier and waits for that state owner | Independently admitted per-actor or per-invocation carriers |
| FINREG | Owner-only claim states and scans of unrelated claims block registration/clear | Persistent registration, ordering, and finalization transactions |
| Trace lifecycle | Side publication and root-CAS-loser abort cleanup now avoid SMR waits; automatic pressure flush still handshakes synchronously | Extend durable completion to general asynchronous retirement/flush |
| Trace stitching | Production stitch probe rejects every edge and the stitch entry returns immediately | Prove C/VM return and snapshot lifetime before enabling real executed stitched traces |
| VM events | START/STOP/ABORT/RECORD callbacks still retain recorder ownership | Exact event/continuation sessions with callbacks outside shared ownership |
| Generic FFI traces | CALLXS is live; first generated callbacks could overwrite loop slots through stale owner stack bounds | XSAVE owner geometry is repaired; continue aggregate ABI, caller topology and no-replay lifecycle work |
| Cdata method recording | Pre-MT guards are repaired and restricted pure loops reuse entry checks; shared-MT constructor/field recording still refuses before trace-owned exceptions | Exact rooted recorder/native lookup and root-publication exclusion remain prerequisites for MT enablement |
| Special userdata dispatch | Mutable methods use common native guards; direct pure pre-MT loops now reuse exact entry metatable/node loads | Keep entry guards and complete-body exclusions; broader captured/upvalue and MT lookup proofs remain |
| C-library lookup | Native lookup guards the original mutable cache and close; retained-cdata comparison reduces measured shared captured-builtin lookup from about 251 to 167 ns | Preserve source authority and continuous trace lifetime; broader MT metamethod recording and stock parity remain open |
| Win64 table traces | The recorder rejects ordinary HSTORE/ASTORE, including pre-MT stores | Complete Win64 helper ABI and require executed table-store traces under Wine and native CI |
| Diagnostics | Remote allocator-list walks are replaced by scalar publication in `abf234ca`; other snapshot lifetime contracts still apply | Preserve owner/lifetime contracts and audit remaining diagnostic access |
| Allocator API | The default internal-allocator gate ignores custom `lua_Alloc` callbacks and makes `lua_setallocf` a no-op | Restore exact allocation ownership and callback behavior as a separate tested milestone |
| Generational GC | Published controls do not prove physical minor collection or all remembered edges | Complete parent-edge audit and prove actual minor-cycle reclamation |
| Performance | Existing 100x cliff gate and historical samples cannot prove parity | Current pinned-stock measurements and the original final gates |

## Corrections to the original algorithms

The table sketch in 06 §6.3.5 freezes a value before publishing its payload and
assigns migration work with an owner cursor. Neither step by itself is
helpable. The flag-gated plain-store sketch in 06 §6.3.2 also has a
check-to-store race: a delayed writer can store after retirement. The existing
private pre-MT path has separate activation exclusion; that does not justify
plain stores in an arbitrary shared generation.

The original string sketch cannot destructively relink one GCstr into old and
new hash topologies while claiming both are concurrently readable. A resize
claim plus draining readers is still owner-dependent. Canonical string
identity and allocation reclamation must share an explicit admission and
retirement proof.

The original safepoint sketch puts waiting on a dedicated GC leader. Today's
automatic mutator drivers also execute that leader path. Moving a synchronous
loop behind a function named `try`, `step`, or `assist` does not fix it.
Similarly, a single worker token is not parallel marking, and reopening the
JIT gate between GC batches is not concurrent tracing of active trace roots.

Generic `IR_CALLXS` is implemented for admitted shapes. The blanket disabled
description in 11 §11.5 is historical. Its production path must retain exact
native-frame rooting and return conversion while extending aggregate and
caller coverage; replace neither with hand-picked foreign-function matches.

## Dependency-led work

### A. Repair demonstrated local violations

The reviewed repairs and checks have landed with deterministic negative
controls and post-fix completion tests:

- `4a46db9e`: atomically publish descriptor control and capacity. Linux GCC,
  Clang, and ASan descriptor fixtures pass; evidence is in
  `notes/table-descriptor-install-pair-2026-09-04.md`.
- `eb77c111`: keep MARK-close requests durable without peer waits or false
  worker progress. The paused-owner fixture, phase/scheduler/JIT cooperation,
  and full traversal fixtures pass; evidence is in
  `notes/runtime-gc-review-2026-09-04.md`.
- `1981938f`: correct pre-existing API publication-hook and recovery-queue
  setup assumptions while preserving and strengthening their assertions.
- `55117337`: remove redundant pre-store parent barriers from complete rooted
  paired stores. The exact post-store/root handoff remains. Seven paired
  insertion measurements found no material speedup; this does not solve the
  traversal cliff. See `notes/table-prestore-barrier-review-2026-09-04.md`.
- `ea23bf0c`: admit side-trace parent/root bodies once before publication and
  abort speculative recording on refusal. Recorder ownership retains admitted
  bodies through linking; optional debugger/perf registration follows linking
  so same-owner GC cannot invalidate that lifetime proof. Real paused-reclaimer
  schedules, inspectable RETRY events, GDB allocation failure, and both restored
  wait negative controls are covered in
  `notes/jit-stop-admission-nonwaiting-2026-09-04.md`. The root-abort follow-up
  below addresses post-publication cleanup; disabled stitching remains open.
- `09d09e63`: preserve public SWEEP table requests after raw writes, and make
  FINREG membership inspection observational with explicit RETRY propagation.
  Available-buffer lost-edge controls, cyclic child graphs, real admission
  refusal, weak/finalizer regressions, and ASan pass. The former predicate's
  semantic publication also caused self-sustaining collector work. See
  `notes/gc-sweep-public-table-rescan-2026-09-04.md`.
- `09cef065`: retain exact SWEEP TValue admission through marking and edge
  publication, removing duplicate admission. Exact small/huge object lifetime,
  type/layout, transient failure, and recovery controls pass. See
  `notes/gc-sweep-edge-admission-reuse-2026-09-04.md`.
- `006da911`: finish successfully preserved string/cdata leaves without
  queueing them as graph work. Marking, failure recovery, userdata/finalizer,
  and cyclic graph obligations remain covered; the leaf-only negative control
  restores the unnecessary queue. See
  `notes/gc-sweep-leaf-publication-2026-09-04.md`.
- `abf234ca`: replace remote allocator-list walks with owner-published arena
  counts and binmask loads. Protected-arena and targeted TSan controls fail
  with the old reader and pass with the scalar reader. Counter transitions,
  rollback, transfer, bootstrap, and partial teardown are covered in
  `notes/gc-stats-arena-publication-2026-09-04.md`.
- Root-CAS-loser cleanup now publishes a pending request on its exact reserved
  slot before the ABORT callback. An eligible root scan consumes it under
  tracked SMR and one-shot recorder admission. Paused-reclaimer completion,
  ordinary trace inspection, callback flush/GC, graph preservation on refusal,
  repeated capacity flushes, real reclamation/slot reuse, close, and the old-wait
  negative control pass. See
  `notes/jit-root-abort-deferred-retirement-2026-09-05.md`. A resumed table read
  still waits on that same paused reclaimer; its stopped stack is preserved as
  a separate core-operation progress defect. The small scalar-hit path below
  addresses one ordinary case; the general read path remains open.
- Semantic table requests now invalidate their scan proof under retained
  admission before publication, allowing covered duplicates to share one
  completed SWEEP scan. The public MARK helper also retains exact admission
  through publication and preserves denied/saturated requests. Small/huge,
  raw-store, phase-crossing, cyclic, protected-memory, recovery, and fourteen
  negative controls pass. Independent review and the integrated assertion/
  ASan runs are recorded in
  `notes/gc-sweep-table-coalescing-2026-09-05.md`.
- `bec8cd2d`: certify physically empty CLOSED traversable spares and retain
  them on a separate atomic reclaimed head. Skipping repeated preparation
  preserves late publisher/reader evidence; allocation still tries only one
  adoption. The first revision was rejected for growing retained capacity.
  Final reuse, paused head-CAS, plain-kind routing, destructor, registry, and
  terminal controls pass, with stable retained capacity in the real workload.
  See `notes/arena-empty-reclaimed-reuse-2026-09-05.md`.
- `23c0c753`: complete positive small number/boolean table hits through exact
  mapping/body leases before general SMR admission. Ordinary interpreted field
  reads finish while the real unrelated IDLE reclaimer is paused. Source and
  vector confirmation, protected pages, aliasing, retirement, ASan, and the
  restored old-wait negative are covered. Huge/GC-result/general meta paths
  and the local plain-arena writer gate remain open. See
  `notes/tab-scalar-hit-admission-2026-09-05.md`.
- `cf1770dd`: strengthen JIT resize coverage with one controller-owned native
  exit observer and exact worker IDs after the concurrent phase starts.
  Global trace-count deltas and per-worker global attachments are unsuitable
  oracles. Eighty positive processes and four required negative controls pass;
  an initial old-oracle failure remains unreproduced and is not claimed as a
  diagnosed JIT defect. See
  `notes/jit-resize-native-exit-coverage-2026-09-05.md`.
- `28de50a6`: return broader positive rooted table hits before allocating
  general metamethod-chain anchors. GC results and Huge vectors retain the
  exact reader/result transfer protocol; absence/refusal leaves aliased inputs
  and outputs untouched. Strict, ASan, stock, canonical meta, concurrent resize,
  and real queue-growth publication checks pass. Queue allocation and general
  SMR admission remain. See `notes/meta-positive-rooted-hit-2026-09-05.md`.
- `5d486412`: avoid positive table-reader attempts when an acquire tag-only
  sample classifies the authoritative receiver as non-table. Exact readers
  still load the original source cells; function environments stay excluded.
  Stock, canonical and paused-reclaimer controls pass. See
  `notes/meta-receiver-tag-2026-09-05.md`.
- `8cea705d`: repair FNEW fallback tests to keep allocator identity truthful
  during actual construction, and test identity refusal through the real pure
  eligibility predicate. Preserve carried SSB/root work instead of relying on
  empty-buffer assumptions. Canonical and strict checks pass; the malformed
  setup fails the new unfinished-constructor assertion. Normal runtime
  preprocessing is unchanged. See
  `notes/fnew-fixture-valid-allocator-2026-09-05.md`.
- `4e7a270e`: capture a callable first cdata method during the existing exact
  receiver/key admission, publish its private root, then close extra scopes
  before generic key publication. Thirteen strict/ASan/LSan schedules and
  ordinary Lua/FFI collection, callback, replacement and alias cases pass.
  Arbitrary allocator callbacks retain the existing path. See
  `notes/meta-cdata-capture-2026-09-05.md`.
- `30cf1d99`: guard mutable cdata base-table methods in admitted pre-MT native
  traces. Eight mutation/lifetime controls require real native exits and exact
  calls/errors; stock and activation cases pass. Retain the measured tiny-loop
  cost rather than the stale-method assumption. Shared-MT recording remains
  refused. See `notes/jit-cdata-basemt-guards-2026-09-05.md`.
- Generated foreign-call entry now publishes validated XSAVE owner base/top
  before native admission. This repairs a confirmed first-callback numeric
  loop overwrite. Scalar callbacks, actual stack relocation/full GC, error
  unwind and cleanup pass in normal/assertion/ASan builds, including the
  current mode-0 poll repair. Existing XSAVE-staging and remote-flush aggregate
  failures reproduce on baseline. The XSAVE and remote-flush fixture corrections
  below close their gates while retaining the separate MT lookup limitation. See
  `notes/ffi-callback-stack-geometry-2026-09-05.md`.
- The XSAVE staging fixture now completes SWEEP and resets fresh poison before
  warming its actual generated producer. Original shape, native owner/frame
  and GC assertions remain unchanged; current-source canonical and isolated
  ASan/LSan checks pass, with disabled-producer and missing-admission negatives.
  No runtime source changes. See `notes/jit-xsave-staging-fixture-2026-09-05.md`.
- Pure pre-MT cdata loops reuse guarded entry method loads only after whole-body
  effect classification in protected unrolling. Entry guards, phase/TG polls,
  call/allocation exclusions and error cleanup remain. Combined normal/assert/
  ASan validation passes 90 processes plus both new shared canonical entries.
  Seven paired current-source field benchmarks recover 0.0684 s to 0.0206 s
  (69.9% less); this is a focused field-loop result. General MT method recording
  remains refused. See `notes/jit-cdata-pure-2026-09-05.md`.
- The authentic CALLXS aggregate now passes its remote GC/flush, post-call and
  nested-callback checks. The remote fixture captures actual builtin dispatch
  before recording and requires each blocked pointer/bool/sret call's return PC
  to be in published mcode. An interpreted-call negative fails that oracle.
  Normal/assert/ASan and the shared canonical aggregate pass. General shared-MT
  metatable/library lookup recording remains refused; no runtime source changes.
  See `notes/ffi-remote-flush-fixture-2026-09-05.md`.
- Exact owner-root/SSB actions can release their native owner after the unique
  remote executor finishes all private accesses. Local winners remain held:
  a real native-depth detach/duplicate schedule exposed an outstanding remote
  scan despite a current ack epoch. The restricted source passes 36 combined
  processes and a new canonical case, preserving old full-root holds and
  subsequent phase/STOPREQ actions. This is one completed-target wait window;
  full asynchronous GC remains open. See
  `notes/gc-remote-root-completion-2026-09-05.md`.
- Special userdata now use the ordinary guarded metatable/method path while
  retaining namespace/subtype specialization. Thirty baseline native mutation
  failures across namespaces, files and buffers are repaired; 326 combined
  processes and the new 88-case canonical registration pass. Pure lookup cost
  increases, including variable file lookup samples, while the actual foreign
  call case changes about 0.4% and `ffi_struct` is unchanged. Independent direct
  CLibrary receiver/cache/close defects remain separate. See
  `notes/jit-special-udata-guards-2026-09-05.md`.
- Captured C-library lookup/store builtins now guard and retain their exact
  namespace before exporting constants or extern addresses. Distinct-library,
  wrong-userdata, installed-side, no-replay and lifetime cases pass 60 isolated
  positives, 54 combined regressions and 20 shared canonical cases. The two
  exact pre-fix controls fail all ten native cases each. Cache mutation and
  semantic close remain separate defects. See
  `notes/ffi-clib-receiver-2026-09-05.md`.
- Direct userdata pure loops now reuse only the exact entry metatable and its
  immediate node load under the existing whole-body proof. All entry/subtype,
  phase/TG, alias and effect exclusions remain; shared MT is not enabled.
  Combined receiver/optimizer validation passes 261 processes and the new
  canonical entry passes 80. The original isolated direct lookup measurements
  improve about 40% for namespaces and 54% for file/buffer; captured receiver
  loops remain excluded and variable file costs remain unexplained. See
  `notes/jit-udata-pure-2026-09-05.md`.
- Native C-library lookup now reads the original cache through raw pre-MT
  guards or the existing bounded shared hit helper, preserves actual numeric
  overrides, guards exact cdata identity, and samples sticky close before use.
  Root/installed-side effects, cache resize/GC, successful native hit followed
  by close and recorder error cleanup pass 753 isolated plus 153 canonical
  processes. The exact baseline fails 58 native semantic controls. Actual
  foreign-call cost changes about 1%; the shared captured-builtin lookup rises
  to 251 ns and remains a substantial performance follow-up. Extra forced
  attachment-during-recording coverage was blocked by automatic safety review
  and is unperformed. See `notes/ffi-clib-cache-authority-2026-09-05.md`.
- Shared C-library lookup now compares acquired cache bits with an exact typed
  cdata KGC retained by the trace, avoiding redundant result discovery and
  publication. Source leases, SMR, owner/generation confirmation, no-replay and
  volatile close ordering remain. Normal/assert/ASan plus shared canonical
  validation passes 1,174 combined processes, including real GC/flush refusal
  and trace-only lifetime controls. Seven matching pairs on top of `84378609`
  measure 251.20 to 167.43 ns, a 33.37% median paired reduction for this lookup.
  Internal CAS retries and broader MT recording remain open. See
  `notes/ffi-clib-cdata-compare-2026-09-05.md`.
- The isolated string baseline separates completed-cycle retention from missing
  automatic progress. Persistent peers or workers retain 2,457,600 string-body
  bytes through 12 explicit completed cycles; sole-main cleanup reclaims them.
  Interpreted allocation can strand a published IDLE request behind the child
  lifetime threshold, while trace completion or last-child detach provides a
  different entrance. Worker-enabled SWEEP completion remains separately open.
  All incomplete cases and counterexamples are preserved. See
  `notes/gc-string-retention-baseline-2026-09-05.md`.
- Public automatic-GC control now uses independent STOPPED and finalizer-pause
  bits. First/last attachment pacing stores cannot reverse STOP/RESTART, and
  safe VM/C boundaries consume durable IDLE requests. Public ISRUNNING reports
  the explicit setting while internal admission retains finalizer suppression.
  Candidate2's query regression is preserved; the final source passes 608
  functional processes including 37 new canonical cases. Another 27 worker-two
  processes still miss SWEEP completion. Seventy matched cost processes show
  no measured increase in selected allocation/arithmetic/FFI cases. See
  `notes/gc-auto-control-2026-09-05.md`.
- The first unfinished-constructor deferral candidate remains rejected. It
  preserves exact construction state and permits completion after publication
  or cancellation, but a real tail constructor monopolizes all 64 subsequent
  drains and a two-worker window while another eligible TG gets no arena
  completion. Preserve this counterexample when adding a bounded continuation
  between owner turns. See
  `notes/gc-construction-defer-review-2026-09-05.md`.
- Constructor deferral now includes a scalar next-TG continuation resolved
  through the current claimed list. Multiple real blockers, quota one and
  actual hinted-TG unlink preserve independent completion before owner release.
  The latest-source combination passes 170 functional processes; 42 paired
  allocation-cost processes show overlapping timings. Same-TG multiple-arena
  fairness, earlier subsystem deferrals and synchronous handshakes remain.
  See `notes/gc-construction-defer-fairness-2026-09-05.md`.
- The hard-assist and allocation-account fixtures now prove their measured
  cycle and SSB-pool setup, and exact meta-store publication counts are backed
  by actual source-stack observations. Ten isolated final processes and five
  canonical components pass without runtime changes. M6 still times out in
  unchanged scalar ITERN under the paused IDLE reclaimer; its last two cases
  remain unrun. Treat that wait as runtime progress work, retaining the paused
  window and original outcomes. See `notes/gc-helper-fixtures-2026-09-05.md`.
- The scheduler terminal-unlink fixture now clears its own synthetic shutdown
  flag before real close admission. The original rejected close and 131,280-byte
  LeakSanitizer report are preserved; eight isolated and three registered
  components pass with the cleanup. Runtime close guards remain intact. See
  `notes/gc-scheduler-close-2026-09-05.md`.
- The local-native duplicate fixture now accepts an exact same-epoch request
  republished between consume and claim. Forced real schedules reproduce the
  old assertion on both original runtimes; all actual completion and teardown
  checks remain. Eight owner, four current-runtime and eleven registered
  processes pass. Runtime acknowledgement rules are unchanged. See
  `notes/gc-native-duplicate-fixture-2026-09-05.md`.
- The ownership-spine EOF call still flushes a whole pending chain despite the
  ordinary prune budget: one real call handles 262,144 exact identities, taking
  about 5–6 ms in three normal observations. Ten normal/ASan controls retain
  payloads, userdata placement and real collection. A whole-chain continuation
  needs shared-consumer ownership, incarnation retention and reentrancy proofs;
  prefix cutting conflicts with unfinished constructor link reads. No runtime
  implementation is ready. See `notes/gc-pending-root-eof-2026-09-05.md`.
- Workers now schedule SWEEP preparation and continue only after a certified
  frontier advance. Closing their startup native scope also preserves consumed
  action lifetime before detach. The current combination passes 129 isolated
  and 58 registered components, with unchanged allocation-cost ranges. A real
  scheduler fixture READY/close race is corrected under actual token authority;
  its original default abort and strict invalid-gate assertion remain recorded.
  The JIT-enabled sole-main case still exceeds its bound on both old and new
  runtimes; same-TG fairness, EOF work and synchronous handshakes remain open.
  See `notes/gc-worker-sweep-2026-09-05.md`.
- `1bce0fa5`: promote exhausted inline table dirty authority into pre-reserved
  persistent wide proof, retaining common stamp/token geometry. Small mappings
  use a dense sidecar plane and Huge mappings use checked tail reservation.
  Old scans, paused publishers, reuse, terminal token-only completion, failure
  and repeated collection are covered in the final normal/assertion/ASan
  combination. See `notes/gc-table-wide-authority-2026-09-05.md`.

The combined normal Linux runtime passes the default stock suite (387 tests
with JIT off, 509 with JIT on). That is semantic regression coverage, not
proof of the concurrent progress, reclamation, or performance requirements.
The leaf/statistics allocator state-churn fixture completes in 63.341
seconds with a longer bound after an earlier 60-second timeout. Diagnostic
rounds advance and collect to IDLE; repeated automatic SWEEP traversal remains
a performance problem in this schedule, not demonstrated nonconvergence. See
`notes/arena-state-churn-progress-2026-09-04.md`. A separate sweep-batching
fixture now explicitly selects the two lifetime words required
by its original exact batch assertions; ten combined and ten pre-leaf control
runs pass with that geometry.
The final coalescing plus deferred JIT retirement normal build completes the
same unchanged state-churn fixture in 5.625 seconds and again passes both stock
suites. These are integrated functional completion observations; use the
separately frozen benchmarks for performance comparisons.
The final arena/scalar combination passes all 30 recorded Linux functional
processes, including 18 helper/assert C fixtures, normal lifecycle/state
churn, both stock modes, concurrent table/JIT/weak/finalizer cases, strings,
and buffers. Unchanged state churn completes in 5.379 seconds. Exact production
and fixture identities, build flags, bounds, and results are in
`notes/linux-integrated-stability-2026-09-05.md`. These functional timings are
not paired performance measurements.
The later final combination through `1bce0fa5` passes 113 test processes in
normal, assertion/helper and target-only ASan/LSan variants, including stock
387/509 in each variant, full current GC/TNEW/FNEW C fixtures, 13 cdata capture
schedules, native mutation guards and concurrent table/weak/finalizer cases.
The shared default mixed build also passes both new Lua fixtures in both JIT
modes. Exact combined identities and the known excluded shared-cdata native
refusal are recorded in `notes/gc-table-wide-authority-2026-09-05.md`.

These repairs do not constitute production resize or asynchronous GC
completion. Continue keeping each protocol change and its exact validation in
a separate reviewable commit.

The practical 32-bit table dirty-authority exhaustion issue now has a validated
replacement. `gc2_table_dirty_bump()` promotes to pre-reserved persistent wide
authority, clearing old coverage before publishing the inline mode. Scanners
retain their captured domain, era and serial; cell reuse resets only inline
coverage and preserves exact token refusal. Ordinary rollover permits later
collection, while full 96-bit era/serial exhaustion and the independent
GC-cycle exhaustion policy retain safe containment. The previous statement
that sustained mutation could not realistically exhaust 32 bits is superseded.

Small mappings append a dense wide sidecar plane, keeping the ordinary 16-byte
entry, token offsets, header and emitted VM/JIT reset indexes unchanged. Huge
proofs use checked physical tail reservation rather than a separate allocation.
Logical payload bounds and published traversable realloc refusal are preserved;
FREE/DEFER_FREE token-only completion remains blind to body proof storage.
Permanent tests cover paused publishers/scanners, same-low-bits era changes,
high-cell reuse, allocation failure, actual protected proof pages, full payload
boundaries and repeated real collection. Combined Linux validation and the
separate storage/performance qualifications are recorded in
`notes/gc-table-wide-authority-2026-09-05.md`.

Design evidence remains accountable: the original 32-byte AoS experiment had
four stale emitted indexes and is invalid selection evidence
(`notes/gc-table-authority-wide-prototype-2026-09-05.md`). The corrected AoS
prototype has correctness controls but no corrected timing claim
(`notes/gc-table-authority-wide-corrected-prototype-2026-09-05.md`). The selected
dense layout retains common CAS64 cost at the expense of doubled reserved
sidecar bytes; the tail avoids Huge metadata heap allocation but can add
virtual space or a resident page. Measurements and adverse cases remain in
`notes/gc-table-dense-overflow-prototype-2026-09-05.md` and
`notes/gc-huge-tail-overflow-prototype-2026-09-05.md`. This does not close broader
nonblocking GC or eventual-reclamation requirements. Next protocol work must
preserve scanner stack authority while replacing native consumed-ack and
automatic phase handshakes with durable asynchronous completion.

### B. Remove measured algorithmic performance cliffs

Treat this as ongoing implementation work alongside the correctness protocols,
not a final benchmark exercise. The pinned-stock review found a severe
full-suite interpreter insertion cliff and SWEEP profiles dominated by table
traversal and per-edge allocation admission. Source review identifies repeated
whole-parent queueing and full-vector scans as an amplification route. Small
filtered insertion sizes have approximately flat per-operation cost, so the
current measurements do not establish a simple quadratic law or isolate all
effects of GC phase, table size, and earlier benchmark cases.

The leaf correction now has seven alternating fresh-process insertion pairs:
the 5,000-key reproducer's median falls from 401,201.4 to 2,475.6 ns/key, with
a paired geometric ratio of 0.006159525. Every process validates the complete
table after collection. This measures that correction against the same
stability/admission fixes; it does not establish general or stock parity.

The fresh full JIT harness completes all 15 rows with a fork/stock geometric
mean of 1.540550898. Closure creation/upvalue mutation and insertion remain
about 59 times stock, and coroutine switching about 9.6 times stock. The fork
interpreter pilot times out at 180 seconds after 4/15 rows, including an
existing-key store cost of 663.94 versus 12.69 ns/op. Do not compute an
interpreter aggregate from that incomplete run. Commands, frozen sources,
raw samples, and measurement limits are in
`notes/gc-sweep-leaf-performance-2026-09-04.md`.

The initial coalescing plus deferred JIT retirement full pilot completes all
15 JIT rows with a 1.425195179 fork/stock geometric mean. Insertion reports
1,827.49 ns/op versus 120.05 for stock, while closures remain 55.44 times
stock. The interpreter reaches six rows before its 180-second limit; existing
stores still cost 662.66 versus 13.05 ns/op, and no interpreter aggregate is
computed. This source predates the subsequently identified MARK table-barrier
repair and empty-arena optimization. Preserve that boundary when comparing
later changes. Commands, source overlays, raw output, and missing rows are in
`notes/gc-coalescing-performance-pilot-2026-09-05.md`.

The closure follow-up separates live-graph work from retained capacity.
Filtered closures remain about 28 times stock with the harness's permanent
8,192-key graph, while an insertion prefix doubles fork closure time. Weak
watchers, string/root counts, and bitmap observations show the large insertion
graph is gone, but hundreds of empty reclaimed arenas repeat preparation and
quarantine each cycle. `bec8cd2d` now retains exactly certified empty CLOSED
spares for reuse while excluding redundant full-plane work. The final paired
closure measurements drop from 3,976.11 to 1,530.41 ns after the insertion
prefix, while all five settled rounds retain exactly 320 arenas. Filtered
closures fall from 1,740.75 to 1,523.25 ns. The control predates the MARK-scope
repair and excludes scalar reads; those boundaries are explicit in the note.
The rejected first revision's capacity growth remains archived. The
384-KiB allocation trigger cap also needs a separate pending-root/progress
audit. Profiles, controls, raw counts, and required proofs are in
`notes/closure-upvalue-performance-diagnosis-2026-09-04.md`.
The completed pacing audit confirms that cap is a heuristic cycle-start limit,
not a bounded pending-chain traversal. It also controls the active hard-assist
schedule. A 600,000-closure automatic-only control completes 146 cycles and 438
root scans with zero new pending roots, zero recovery, and 16 traversable arenas
at all 12 sampled endpoints. Real body reclamation still requires those cycles.
Keep production pacing unchanged; any isolated cycle-start comparison must
preserve the existing hard thresholds and active progress cadence. See
`notes/gc-allocation-pacing-audit-2026-09-05.md`.

`23c0c753` reduces the measured positive scalar interpreter paths by about
35–45% in seven alternating fresh-process pairs per case, with exact old-path
controls, GC enabled, and validated results. This establishes the new helper's
cost for small field/array hits, not general table performance or stock parity.
The first combined full JIT pilot reports a 1.320666406 fork/stock geometric mean
over all 15 rows, with closures at 1,598.75 ns/op (21.83 times stock). The
interpreter again times out at 180 seconds after six rows; its aggregate is
undefined. A separate seven-pair matched diagnosis finds 11–25% overhead from
failed scalar attempts in three large-table workloads, while the small array
case improves by about 33%. Preserve those regressions alongside the wins.
See
`notes/linux-integrated-performance-2026-09-05.md`.
`28de50a6` then reuses the existing bounded reader for broader positive hits
before chain allocation, preserving unchanged failure inputs and exact GC
result publication. Seven matched pairs per case reduce existing-key store,
hash-read, and existing-key read costs by about 63%, 29%, and 51%, with the
small array case approximately unchanged. The full JIT pilot has a
1.337883709 fork/stock geometric mean. Its 360-second interpreter timeout is
preserved separately from a fresh full interpreter run which completes all
15 rows in 727.427 seconds under a 900-second bound; compared with the earlier
stock sample, the complete interpreter ratio is 9.694639005. This is a shared
host exploratory observation, not repeated paired acceptance evidence.
Generic interpreter FFI, coroutine, and string-buffer costs remain large. See
`notes/meta-positive-rooted-hit-2026-09-05.md` and
`notes/linux-rooted-hit-full-performance-2026-09-05.md`.
The following tag-only classification in `5d486412` reduces interpreter FFI
cost by about 4.4% across seven matched process pairs, with the four table
cases approximately unchanged. Most generic FFI admission cost remains;
no broad FFI or full-harness speedup is inferred from that narrow measurement.
See `notes/meta-receiver-tag-2026-09-05.md`.
`4e7a270e` removes repeated first-hop cdata admission with exact method rooting.
Seven matched pairs reduce the filtered interpreter FFI cost from 1497.51 to
1241.17 ns/iteration (paired geometric ratio 0.82865); table controls stay
within about 0.7%. See `notes/meta-cdata-capture-2026-09-05.md`.
The separate native stale-method repair `30cf1d99` increases tiny `ffi_struct`
cost from roughly 0.69 to 2.28 ns/iteration (+232% paired median); constructor
cost rises 66% when sunk and 1.2% when materialized. Repeated checks after XPOLL
are visible in the recorded IR. Keep the correctness guards; any reduction
needs explicit pre-MT alias, callback, collection, activation and side-exit
proof. The old full-harness ratios do not measure these newer combined changes.

Continue removing demonstrably redundant publications while preserving receiver
roots and exact post-CAS key/value handoff. Then replace unbounded whole-object
work accounting with durable traversal progress measured in slots or bytes.
An ordinary allocation must not inherit a full huge-table scan merely because
the worker budget says one object. Audit repeated object admission/lease work
alongside queueing and scan-frequency costs, retaining the exact admission
through marking, payload access, and durable publication. Compare optimizations
against the same correctness fixes so dropped work cannot look like a speedup.

Do not apply a worker's clean-table scan shortcut to arbitrary public barriers:
some existing raw writers rely on those barriers to force discovery without a
dirty-epoch update. Each elision needs a complete publication-path proof, weak
and phase-transition tests, and before/after measurements. Keep the full-suite
and filtered workload results distinct; the latter cannot erase a severe
phase/history-dependent cliff in ordinary execution.

### C. Restore eventual string reclamation and asynchronous GC boundaries

Treat both as primary correctness requirements. String churn cannot be
accepted merely because table/trace objects are reclaimed. Introduce a
deterministic live-set/heap-plateau test with secondary Lua threads and GC
workers continuously present; require completed cycles and actual reclaimed
string bodies while preserving canonical identity and anchored strings.

For GC, split each handshake into request, per-participant completion, and
phase-close evidence retained in shared state. An allocation/step may publish
or help bounded work and return while a peer is paused. A later participant
must finish the same request without the original driver stack. Cover attach,
detach, native return, STOPREQ, active finalizers, nested GC, and saturation.
Do not remove consumed-ack waits until the scanner's stack access has a safe
replacement. Then remove global trace exclusion from marking using the exact
native/trace frame evidence, and distribute marking work among real workers.

The integrated automatic-control repair separates public STOP/RESTART from
derived first/last attachment thresholds and retains an independent finalizer
pause. Safe VM/C boundaries now consume pending IDLE requests. The earlier
unsafe admission and STOP-veto prototypes, lost-RESTART schedules, and the
later rejected finalizer query remain immutable evidence. Public ISRUNNING
now reports the explicit setting consistently across actors; internal automatic
admission still observes the pause. See `notes/gc-auto-control-2026-09-05.md`.
The worker-two SWEEP preparation gap is now repaired, and unfinished-owner
deferral retains turns for other TGs. Next resolve the sole-main JIT assistance
bound, fairness between arenas of one TG, and the whole pending-chain EOF tail
while preserving eventual reclamation and useful progress for other owners.
Then replace the synchronous driver and borrowed root/action completion
protocols. See `notes/gc-worker-sweep-2026-09-05.md` and
`notes/gc-pending-root-eof-2026-09-05.md`.
The scheduler SSB-empty assertion now reproduces on all three frozen variants:
the fixture ignored a refused owner flush. It now establishes publication
before the unchanged worker-drain checks; 60 corrected runs and six negative
controls validate that precondition. See
`notes/gc-scheduler-publication-2026-09-05.md`. The later READY ownership
correction and worker SWEEP repair preserve those checks; other recorded waits
remain open.

The real four-position consumed-ack probe and finite mode-0 attachment probe
are recorded in `notes/native-progress-boundaries-2026-09-05.md`. The missing
mode-0 TG poll is repaired in `notes/jit-first-attach-poll-2026-09-05.md`:
normal and assertion tests now require actual early native exit for both
optimized and optimizer-disabled loops. Focused paired costs are unchanged
within these samples. The baseline active-worker `setmetatable` tracing gate
still reports NYI; it is retained as a separate recorder coverage gap. A current
`hs_epoch_ack` is an execution claim, not completion: the real SSB action can
still be paused after it. The precise `SCAN_OWNER_ROOTS|FLUSH_SSB` class now
permits early release only by its unique remote executor after all private
accesses. Local native-depth polls can win a duplicate epoch while a remote
pre-claim scan remains active; their hold must stay. The real teardown-overlap
counterexample, restricted fix, later tail authority, new SSB suffix and phase
controls are in `notes/gc-remote-root-completion-2026-09-05.md`. Full global
root scans still read recorder scratch geometry, and trace/allocator/phase
actions retain separate holds. Further completion state needs its exact
consumer and a futex predicate that cannot lose the wake.

Automatic phase conversion additionally needs durable MARK initialization
with no early worker admission or reset replay, a persistent exact-cycle root
snapshot continuation, and a SWEEP retirement frontier tied to completed
grace. Releasing `worker_active` around the existing synchronous calls without
those replacements would discard their current exclusion proof.

### D. Complete table resize in production

Prepare successor storage and exact move records before owning table
exclusion. Revalidate the exact generation after install; cancel safely if it
changed. Make the first irreversible source mutation imply that all remaining
work is allocation-free and recoverable by another participant.

Before enabling markers, prove source/value/token identity, nil and repeated
value ABA, delayed old writers, cancelled helpers, canonical nil key claims,
collision suffixes, array/hash transfers, exact completion, and generation
cutover. GC must preserve the strong parts of old/successor/intent payloads
without making weak entries strong. Hold old storage through both table-reader
and native execution grace before releasing VM exclusion.

The immutable per-source capture/whole-attempt-abort approach described in the
table review is a candidate to model, not an approved implementation. A mutable
intent with a separate capture-owner state recreates the suspended-owner
problem. A fixed intent array without a source-CAS ABA proof is insufficient.
Publish a modeled and tested production transaction rather than accumulating
unconnected state-machine scaffolding. Then remove owner waits from getters,
setters, length, iteration, and compound library operations while retaining
their ordinary Lua semantics and JIT replay behavior.

### E. Remove FFI and JIT ownership dependencies

Use private CType writer transactions so a paused `cdef` does not block old
committed types. Cover forward declarations, failed definitions, name lookup,
full accepted type-string grammar, conversion, calls, and callbacks. Convert
FINREG as a distinct ordering transaction, and allow concurrent foreign calls
to the same Lua callback through separately owned execution states.

Keep recording opportunistic. Admit all fallible/SMR work before irreversible
trace publication; abort speculative recording on refusal. Define logical
flush/invalidation independently from eventual code reclamation. Move terminal
events and then resumable START/RECORD events outside recorder ownership using
exact session state. Extend SysV and Win64 aggregate ABI lowering, rollback,
varargs, sret, root calls, protected/continuation frames, and tailcalls while
proving foreign side effects execute exactly once.

The shared-cdata hammer's native assertion currently fails because the general
metamethod recorder fence runs before the trace-owned cdata exceptions. A
trace-owned result does not own the shared base metatable or the constructor's
ctype receiver. Do not exempt cdata from that fence merely to satisfy tracing.
Provide authoritative source-root capture, exact receiver/table/method retention
and private root publication before recorder work can throw or allocate. Native
dispatch also needs nonwaiting guards and safe replay at each affected semantic
lookup. Base-root mutation exclusion must span publication; a flush returning
before the root store is insufficient for a newly admitted peer recorder.
The demonstrated pre-MT stale-method defect is repaired separately and does
not establish this shared-MT protocol.

Production stitching is currently disabled in both `lj_trace_stitch_probe()`
and `lj_trace_stitch()`. A side-publication fix or a fixture which directly
enters a dormant stitch branch does not establish stitched execution. Restore
it only with the C/VM return, snapshot reconstruction, GC and no-replay proof.

Keep the temporary internal-only allocator policy visible as incomplete API
compatibility. Its registry, ownership, realloc/retirement and callback
requirements are recorded in
`notes/lua-alloc-temporarily-disabled-2026-07-10.md`; an unrelated GC optimization
must not silently enable those unfinished paths.

### F. Close performance and semantic evidence; validate platforms for release

Run representative benchmarks as each live protocol changes, not only after
all correctness work. Keep stock baselines pinned to a source revision and
report raw samples, compiler flags, CPU affinity, wall/process time choice,
scale, and failures. The current review baseline is exploratory evidence;
full acceptance also needs active-MT and GC/native overlap workloads.

Validate current changes on Linux. Before the next release, resume Wine/UCRT
and Darling validation with exact current artifacts, supplemented by native
macOS and Windows CI. Smoke tests do not prove ABI/unwind, concurrent GC, or
performance equivalence. Preserve the recorded platform failures as release
requirements while following the user's Linux-first sequencing.

## Completion evidence

All rows remain open until current evidence establishes the entire row.
Use the original milestone tests where they still test the intended behavior;
extend tests that only verify temporary fallback or owner serialization.

| Requirement | Acceptance evidence |
| --- | --- |
| Shared Lua semantics and racy safety | Stock interpreter/JIT suites, bytecode/API compatibility, adversarial mixed-value races, metatables, cells/closures, coroutine/debug operations, unwind and finalizer tests |
| Nonblocking core operations | Public operations complete with relevant publisher/scanner/recorder/reclaimer paused at every side-effect boundary; source call-path audit explains every remaining wait |
| Concurrent and parallel GC | Cycles and traversal progress with ordinary mutators and certified native trace frames active; multiple workers demonstrably process independent work; no hidden STW/owner gate |
| Eventual reclamation | Allocation/destruction and retained-byte evidence for strings, table vectors, intents, traces/mcode, CType versions, callback carriers, and retired TGs across repeated cycles |
| Weak/finalizer semantics | Exact weak live/dead sets, no accidental strong intent edges, resurrection, replacement/clear races, registration order, run-once callbacks, callback-spawn and nested-GC behavior |
| JIT completeness | Real executed root/side/stitch traces through shared operations and generic FFI; paused-owner invalidation/publication; snapshot restoration and no side-effect replay |
| FFI completeness except cdef serialization | Committed-type operations progress during paused cdef; same-callback foreign concurrency; SysV/Win64 full admitted ABI and caller topology; raw-memory races preserve VM safety |
| Allocator and minor collection | Owner-safe accounting/diagnostics, physical reuse/unmap proof, all parent-qualified remembered edges, actual minor reclamation and major-after-minor oracle |
| Memory tooling | Appropriate C TSAN drivers with zero suppressions, full-VM limitations documented, ASan/UBSan, assertions/paranoia, torture and sustained mixed soaks |
| Single-thread performance | Same-host pinned-stock JIT geomean <=1.10 (stretch <=1.05), individual cliffs explained and repaired; interpreter and sticky-MT costs also reported |
| MT performance and GC latency | 1/2/4/8 scaling, >=6x on 8 cores for arithmetic/shared reads, allocation/intern/write/channel throughput, mutator-observed poll P99 <500 us |
| Generational benefit | Physical minor collection reaches >=1.5x allocation-churn speed versus full-cycle mode with zero-diff correctness oracle |
| Platforms and delivery | Current x64 Linux/macOS/Windows builds, native CI plus Darling/Wine runtime/ABI/concurrency gates, documented artifacts, committed and pushed code and evidence |

No source-count checklist, test name, passing 100x comparator, or estimated
percentage can replace these proofs. Record failures and missing evidence;
keep the full project goal active until every requirement is satisfied.

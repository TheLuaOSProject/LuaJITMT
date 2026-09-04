# Lockless completion plan

Reviewed: 2026-09-04, starting at `a649f737`.

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
| Descriptor installation | Separate capacity-shadow store can corrupt a winning descriptor before the losing ownership CAS | Publish control and capacity in one atomic pair; deterministic competing-generation test |
| Automatic GC | Allocation-driven MARK/root/sweep boundaries still execute synchronous handshakes | Persistent asynchronous phase requests and helper-owned completion |
| Native acknowledgement | Native return can wait for a leader to clear a consumed poll/ack | Immutable scan evidence and independently completable acknowledgement; preserve stack exclusion |
| Worker scheduling | MARK-close ownership loss can be reported as progress and cause repeated drain-loop execution | Return/defer without false progress or peer sleep; preserve durable retry |
| GC work per mutation | Public SWEEP barriers can repeatedly queue an entire growing table; traversal charges a whole vector as one work unit | Prove redundant barrier elision, bound traversal by slots/bytes, and measure full-suite phase/history amplification |
| Strings | Physical body reclamation requires explicit collection by the sole main TG with no GC workers | Concurrent canonicalization, unlink, and eventual body reclamation |
| String interning | Header resize claim waits for pinned readers and blocks entrants | Immutable successor topology and helpable publication |
| Marking/JIT overlap | One worker token serializes tracing; each mark quantum excludes every active `jit_base` | Parallel mark ownership plus certified concurrent native/JIT roots |
| CType state | General FFI readers wait for the parser sequence, including existing user-defined types | Private parser transactions with immutable committed versions |
| Foreign callbacks | Same callback slot shares one hidden carrier and waits for that state owner | Independently admitted per-actor or per-invocation carriers |
| FINREG | Owner-only claim states and scans of unrelated claims block registration/clear | Persistent registration, ordering, and finalization transactions |
| Trace lifecycle | Side/stitch publication waits for SMR; automatic pressure flush handshakes synchronously | Try/abort before publication, durable completion and asynchronous retirement after it |
| VM events | START/STOP/ABORT/RECORD callbacks still retain recorder ownership | Exact event/continuation sessions with callbacks outside shared ownership |
| Generic FFI traces | CALLXS is live, with incomplete aggregate ABI and caller topology | Extend generic ABI lowering and no-replay frame lifecycle |
| Win64 table traces | The recorder rejects ordinary HSTORE/ASTORE, including pre-MT stores | Complete Win64 helper ABI and require executed table-store traces under Wine and native CI |
| Diagnostics | `gcstats()` walks another TG's owner-local allocator lists | Owner-published scalar evidence or a retained bounded snapshot |
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

The combined normal Linux runtime passes the default stock suite (387 tests
with JIT off, 509 with JIT on). That is semantic regression coverage, not
proof of the concurrent progress, reclamation, or performance requirements.

These repairs do not constitute production resize or asynchronous GC
completion. Continue keeping each protocol change and its exact validation in
a separate reviewable commit.

### B. Remove measured algorithmic performance cliffs

Treat this as ongoing implementation work alongside the correctness protocols,
not a final benchmark exercise. The pinned-stock review found a severe
full-suite interpreter insertion cliff and SWEEP profiles dominated by table
traversal and per-edge allocation admission. Source review identifies repeated
whole-parent queueing and full-vector scans as an amplification route. Small
filtered insertion sizes have approximately flat per-operation cost, so the
current measurements do not establish a simple quadratic law or isolate all
effects of GC phase, table size, and earlier benchmark cases.

First remove demonstrably redundant publications while preserving receiver
roots and exact post-CAS key/value handoff. Then replace unbounded whole-object
work accounting with durable traversal progress measured in slots or bytes.
An ordinary allocation must not inherit a full huge-table scan merely because
the worker budget says one object. Audit repeated object admission/lease work
only after the queueing and scan-frequency costs are understood.

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

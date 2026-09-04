# Runtime GC review and MARK-close scheduling correction

Reviewed source: `a649f737` (`tab: add persistent resize descriptor substrate`),
plus the working-tree correction described below. Earlier dated notes are
orientation, not evidence that a production cutover or acceptance gate passed.

## Correction in this change

An ordinary failed MARK-close attempt had two avoidable scheduling costs:

1. `gc2_worker_drain_inner()` returned one completed work unit when
   `mark_close_intent` remained published after a failed helper attempt. If a
   peer was suspended while holding `worker_active`, a parked worker's inner
   loop repeatedly obtained that synthetic unit and never reached its park
   decision. This was owner-dependent CPU spinning, and the returned progress
   did not represent a completed GC operation.
2. `gc2_mark_complete_result()` entered `gc2_peer_wait_l()` after an incomplete
   attempt. Automatic allocation GC calls this through
   `gc2_step_auto()` -> `lj_gc2_step_explicit(L, 1)`, so even a one-unit
   automatic step could enter native mode and sleep on the peer's worker word.
   Native leave could then reach the existing synchronous handshake wait.

The worker now returns zero for that unchanged MARK frontier. The common
MARK-close helper returns its incomplete result immediately. It has already
published the close intent, which remains available across ownership loss,
active native frames, and an owned but incomplete bounded round. An incomplete
round still reopens the ordinary MARK scheduling gate. Later helpers honor the
existing native scheduling lease before requesting exit again. The intent is
consumed only by the serialized phase transition/reset. Full-collection and
multi-unit explicit-step drivers retain their existing outer retry decisions;
no completion is forged and no pending work is discarded.

Keeping the request across bounded misses is necessary: with only the first
two scheduling corrections, the new fixture deterministically stalled after
the first owned round added roots. MARK remained active with no close intent,
so the worker had no request to drive the subsequent zero-mark fixpoint round.
The completed correction preserves that request and its native scheduling
opportunity together.

This does not require waking another worker on every failed claim. The current
`gc2_worker_main()` samples `worker_wake` before draining and rechecks it before
parking. Its active-phase 10 ms timeout retries MARK even if the owner
releases only `worker_active` and its separate futex. The failed helper neither
changes that wake sequence nor clears the close intent. Repeated ownership
loss therefore reaches the park decision, while an eventual owner release
still permits the original request to finish. This preserves the existing
idle behavior without adding a hot owner-release syscall or global counter.

The new `tests/t-gc2-mark-close-progress.c` exercises the production driver and
parked worker using the existing atomic ownership accessors. It checks:

- the automatic allocation driver, one-unit explicit step, and direct
  MARK-close call do not enter native mode or retry-yield while the exact
  worker token remains held; linker wrappers observe actual calls without
  replacing their behavior or relying on a latency threshold;
- direct worker drains return zero while the unchanged owner is suspended;
- a real worker reaches repeated park attempts without accumulating fabricated
  asynchronous progress;
- the same worker preserves an explicitly held native scheduling lease while
  the close request remains pending;
- releasing only the ownership word and its futex lets the original durable
  intent complete without another explicit request or publication wake; and
- rooted parent/child data survives completion of the resulting GC cycle.

The fixture is registered as `m3_gc2_mark_close_progress`, included in the M3
scaffold dependency list and its explicit fixture sequence. It links the
normal static library with `--wrap=lj_native_enter` and
`--wrap=lj_thr_retry_yield`; no new production pause hook is added.

The existing `t-gc2-phase.c` peer/assist fixtures now require the deferred
attempt to leave the wait counter and observed native state unchanged, while
retaining their subsequent root-scan and phase-completion assertions.

`t-gc2-traverse.c:test_proto_chunkname_publish_barrier` now pins the automatic
GC threshold after its explicit MARK start, matching the adjacent closure and
buffer barrier fixtures. Its raw SSB drain intentionally yields to close
intent and is reachable only through `lj_gc2_test_ssb_drain`, not a production
driver. Isolating this publication test allows stronger unconditional MARK
and chunk-name marking assertions while preserving its empty-SSB and cycle
completion checks. Production workers and explicit drivers already help the
pending close request.

Linux x64 validation passed with a consistent forced rebuild. The archive
contained this GC correction and the table-source changes subsequently
committed as `4a46db9e`; it did not contain the proposed `lj_meta.c` publication
elision. The tests below therefore establish the scheduling correction's
behavior before that separate performance change:

```sh
make -C src -j4 -B XCFLAGS='-DLJ_TAB_TEST_HELPERS -DLJ_GC2_TEST_HELPERS'
cc -std=gnu11 -O2 -Wall -Wextra -Werror -mcx16 -Isrc \
  tests/t-gc2-mark-close-progress.c src/libluajit.a -lm -ldl -pthread \
  -Wl,--wrap=lj_native_enter -Wl,--wrap=lj_thr_retry_yield \
  -o /tmp/lj-gc2-mark-close-progress
timeout 20s /tmp/lj-gc2-mark-close-progress
```

- The new fixture passed 25 consecutive runs after the consistent rebuild.
- `t-gc2-phase`, `t-gc2-worker-scheduler`, `t-gc2-jit-mark-coop`, and the full
  `t-gc2-traverse` fixture passed against that same archive. The first three
  were relinked and rerun after the traversal-test correction.
- Strict GCC/Clang C11 syntax checks of the new fixture and `git diff --check`
  passed.
- The canonical `tools/ci/lua_test.sh m3_gc2_mark_close_progress` registration
  passed after rebuilding the default runtime. Its full runner output is in
  `/tmp/lj-gc-review-suite.log`.
- Restoring just `src/lj_gc2.c` from `a649f737` into an otherwise identical
  temporary static archive caused the new fixture to fail at the actual
  native-entry assertion. Restoring that source with only the peer-wait call
  removed caused it to fail independently at the direct worker-drain
  zero-progress assertion. These are isolated source controls, not claims of
  a full original-tree runtime build. Their source, objects, archives, and
  failure output are retained in `/tmp/lj-gc-negative-reo6ymqv/`.

Relevant implementation locations are `src/lj_gc2.c:21977` (worker result),
`:22368` (durable helper request), and `:22448` (common incomplete return).

## Remaining production blockers, ranked

### 1. Automatic GC and native returns still contain synchronous peer waits

`lj_gc_step()` reaches synchronous `lj_gc2_handshake()` calls at MARK
activation (`gc2_mark_begin`), root snapshots (`gc2_mark_root_snapshot`), and
sweep grace (`gc2_worker_sweep_progress`). In `src/lj_safepoint.c`,
`safepoint_leader_enter()` waits on `hs_leader`, and
`lj_safepoint_handshake()` waits on `hs_pending` while another TG may be
suspended. `safepoint_wait_consumed_ack()` can park an ordinary native return
indefinitely until the leader clears its poll. The held-poll policy covers
roots, SSB, allocator and trace actions.

Source references: `src/lj_gc2.c:4070`, `:22187`, `:21792`;
`src/lj_safepoint.c:64`, `:790`, `:840`.

These are live allocation/native-return call chains, contradicting the
mutator-never-waits claim in `plan/05_gc_concurrent.md`. The small scheduling
correction above does not remove these waits. The required cutover is durable
asynchronous action and completion state for every phase boundary, with
owner-published root/allocator snapshots or independently revocable native
admission. Removing the waits while retaining remote mutable-stack scanning
would be unsafe.

Acceptance must suspend the publishing leader, a requested non-native TG, and
a remotely scanned native TG at their actual ownership boundaries. Unrelated
ordinary operations must complete, and deferred phase/destructor work must
remain pending until its real proof is available.

### 2. Automatic/shared string bodies are retained indefinitely

`lj_str_gc2_sweep_begin()` in `src/lj_str.c` hard-gates physical string sweep
through `str_reclaim_exclusive_try()`. Admission requires an explicit full-GC
request, the main TG, and zero secondary mutators, entrants and configured GC
workers. Automatic GC never admits the pass, and a persistent multithreaded
application cannot admit it even through explicit collection. The concurrent
canonical quarantine/commit protocol remains hard-disabled.

Source references: `src/lj_str.c:1254`, `:1727`.

This is a functional heap-growth boundary, not just an optimization. Promote
concurrent string-body reclamation to an explicit release blocker. Test
bounded-live-set unique-string churn under automatic GC with live peers and
workers; completed cycles must produce a stable retained-heap envelope while
canonical string identity and old-reader safety remain intact.

### 3. The collector remains serial and globally alternates with native work

All grey/weak/sweep work uses one `worker_active` ownership word and one
global grey deque. The capped two-worker pool does not provide two simultaneous
marking workers. `gc2_worker_drain_inner()` closes native trace entry before
each MARK traversal quantum and declines the whole quantum if any TG still
publishes `jit_base`; a peer blocked in traced FFI therefore delays all marking.
MARK and SWEEP reopen entry between batches, including 50 us scheduling
windows, so describing JIT as disabled throughout a collection would be wrong.
The implemented behavior is global alternation, short of the concurrent
parallel design in `plan/05`.

Source references: `src/lj_gc2.c:1460`, `:21943`.

Separate phase-close authority from independently claimable tracing and
destruction work. Complete exact native-frame/root admission and memory
reclamation proofs before relaxing the JIT gate. Acceptance needs both
simultaneous marking-worker progress and heap traversal while another TG stays
inside a certified traced foreign call; merely starting two threads is not
parallel-collector evidence.

### 4. String-table topology still blocks ordinary interning

`strtab_claim()` claims the live header and waits for every existing active
reader. `strtab_enter()` makes interners wait for that resize owner.
`strq_enter()` similarly waits on quarantine-header replacement. These paths
reach `strtab_wait()` / `lj_thr_retry_yield(L)` from ordinary string creation.
Implement immutable successor topology and a helpable publication protocol so
neither a pinned reader nor a suspended rehasher blocks another interner.

Source references: `src/lj_str.c:497`, `:526`, `:592`.

### 5. Remaining SMR readers still wait behind an exclusive reclaimer

`lj_gc2_smr_read_enter()` loops on `lj_gc2_smr_read_try()` using
`lj_thr_retry_yield(NULL)`. Current callers remain in GC, safepoint, and trace
code. Replacing a blocking call requires preserving that consumer's actual
root/identity contract on admission failure; plain omission of the lease or
an unbounded retry loop is not a nonblocking conversion.

Source reference: `src/lj_gc2.c:6965`.

### 6. Diagnostic allocator snapshots have a cross-owner data race

`threading.gcstats()` calls `lj_gc2_stats_snapshot()`, which reads the main
TG's `alloc.owned[]`, `needsweep[]`, and `binmask[]` and walks the arena lists.
`lj_arena.c` modifies those owner-local fields plainly during allocation and
sweep. A secondary Lua caller can therefore race the owner. Publish scalar
diagnostic counts at owner boundaries or provide a bounded owner-produced
snapshot. Atomic loads alone do not establish the list's topology/lifetime
proof, and a cold diagnostic API is still covered by unconditional safety.

Source reference: `src/lj_gc2.c:6768`.

### 7. Performance and completion gates do not yet prove the target

`tests/suites/m9_m10_gc.lua` still defaults to a 100x geomean comparison ceiling.
That only catches catastrophic regression. Both physical-minor collection
gates initialize disabled in `lj_gc2_init()`, and the source still contains
staged single-owner and sole-mutator machinery described above. Dated
percentage estimates and helper/model tests cannot establish production
reachability, bounded completion, reclamation, or speed parity.

The current release scope is Linux, per the latest user instruction; defer
macOS/Windows implementation and validation until the next release.
Replace stale plan claims with verified implementation boundaries; require
reproducible stock comparisons, a multithreaded GC-churn throughput floor,
retained-heap measurements, and deterministic paused-owner schedules in
addition to ordinary functional/sanitizer runs. Tighten the performance gate
as measured regressions are removed rather than treating its current success
as evidence of parity.

## Measured performance priority: repeated table tracing during SWEEP

The performance reviewer measured interpreter `tab_insert_newkey` at
174761.75 ns/op against stock 139.02 ns/op, approximately 1257x, using the
unmodified `a649f737` baseline. Its sampled stack was
`lj_meta_cat -> gc2_step_auto -> lj_gc2_step_explicit ->
gc2_worker_drain_inner -> gc2_traverse_tab_rec -> gc2_trace_sweep_tv_edge`.
Raw profile evidence is in
`/tmp/lj-runtime-performance-review-2026-09-04/pilot-fork-interp-perf-report.txt`.
The MARK-close correction above changes no SWEEP path, and the original
baseline already exhibits this cost.

Source inspection supports a likely quadratic amplification, which still
requires controlled measurements to attribute:

- `gc2_traverse_tab_rec` (`src/lj_gc2.c:18547`) walks the entire array/hash as
  one work item. It has no retained slot cursor or slot budget, and a dirty or
  forwarded generation restarts the full scan later.
- Ordinary table publication reaches `gc2_trace_sweep_edge`
  (`src/lj_gc2.c:21689`), which republishes a traversable parent even when a
  previous scan is current. Only the admitted worker-child path filters an
  already covered table. Complete keyed store handoff also dirties and queues
  the parent (`src/lj_gc2.c:16203`). Repeated appends can therefore rescan an
  increasing prefix.
- Each `gc2_trace_sweep_tv_edge` acquires an exact TValue lease, then calls a
  sweep-preserve path which admits and retains the same candidate again
  (`src/lj_gc2.c:16487`, `:21689`). The profile includes both paths under a
  table edge. This increases each repeated scan's cost.

The smallest substantive performance follow-up is to identify complete keyed
stores whose existing root retention and post-store handoff already supply
the same publication guarantee, then remove only the duplicate parent
publication at those proven call sites. Measure insertion at several table
sizes with GC enabled and disabled, retaining the weak-table and concurrent
mutation fixtures. That local cleanup alone cannot prove linear behavior:
parent rescans and full-object work budgeting still need explicit design.

Do not globally skip the public barrier merely because a table's scan stamp
looks current. That path explicitly covers callers which can mutate raw
payload without updating the dirty epoch, and NEEDSCAN is not exact rescan
token ownership. Reusing an existing child lease likewise requires threading
its identity and recovery proof through the preserve operation, not deleting
admission checks around a raw pointer. These performance changes are review
findings only and are not implemented in this scheduling correction.

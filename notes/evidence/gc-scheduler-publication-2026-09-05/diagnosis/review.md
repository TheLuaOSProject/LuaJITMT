The worker scheduler assertion is reproducible on the pristine 597b control,
the initial admission candidate, and the six-file STOP-veto candidate. Seven
new diagnostic failures identify the same unmet fixture precondition: one
main-owner SSB entry remains private after the fixture's single flush attempt.
Background workers have drained every published queue, their own private
suffixes, and the grey deque. The five failures that also record the existing
flush return all record zero. This explains those new failures and makes the
assertion baseline-reproducible. The original 1.2155-second failed run did not
capture its internal state; its exact cause is not retroactively observed.

No shared source or build was edited. This review uses the frozen archives
named in inputs.json, all with matching assertion/API checks and the same six
test-helper macros. No new runtime build, gate, phase state, worker ownership,
native certificate, scheduler hook, or timeout was introduced. The existing
fixture already contains its own synthetic phase and worker-token subtests;
those remain byte-for-byte unchanged in both diagnostic generations.

The first diagnostic generation inserts output only after the original
wait_ssb_empty loop has exhausted all 1000 existing 1 ms sleeps. The second
also stores the already-returned value of the existing explicit flush in
test_async_mark; it does not make an additional runtime call on that path.
Both retain every assertion and every original polling predicate. Output
snapshots are individually acquired observations, not a simultaneous global
snapshot. They do not dereference a worker-private queue chain. TG bodies stay
owned by the fixture's live worker set until its later shutdown subtest.

Each generation ran a fixed 20 unpinned, fresh, sequential processes per
variant, interleaving control/candidate/veto. No artificial load or scheduling
affinity was added. The existing outer subprocess limit remains 60 seconds;
core dumps are disabled to avoid unrelated binary files. All six compiles
passed. Across 120 runtimes, 113 passed and seven reached the original SSB-empty
assertion. No runtime timed out and no other assertion failed.

| Generation | Control | Initial candidate | STOP veto |
| --- | --- | --- | --- |
| Failure-only output | 19 pass / 1 fail | 20 pass | 19 pass / 1 fail |
| Flush return retained | 17 pass / 3 fail | 19 pass / 1 fail | 19 pass / 1 fail |

The first generation's failures are veto-17 and control-18. The second's are
return-observation/veto-00, candidate-01, control-11, control-13 and control-18.
All stdout/stderr, exact commands, environment additions, fixture/archive/ELF
hashes, bounds and statuses are recorded in the two results.json files. A
read-only convenience pgrep check returned 1 because no matching process
remained; that shell-query result is not a fixture failure or pass.

Every failure reports MARK cycle 2, leader zero, root snapshot zero, no
MARK-close intent, no pending handshake, worker_active/assist_active zero,
two live non-stopped workers, published SSB head/drain null, consumer count
zero, grey indexes 8/8, no recovery identities and no fail-closed recovery
veto. Both worker SSB suffixes are empty. Main's ssb_next exceeds ssb_base by
exactly one GCRef (8 bytes), and its spare node has since been recycled.
The graph-mark wait immediately before the failed assertion had succeeded.
The diagnostic does not capture which transient refusal branch was taken
inside the flush call. A busy worker token is a source-supported explanation;
the exact branch is not claimed as a directly observed fact.

The relevant contracts are visible in the copied veto sources:

- tests/t-gc2-worker-scheduler.c:1231 constructs parent -> child -> grandchild,
  starts MARK, marks parent, ignores lj_gc2_flush_ssb's result at line 1263,
  then waits observationally for grandchild and global SSB emptiness. The wait
  at line 202 calls the empty predicate and sleeps, without publishing the
  owner's private suffix.
- src/lj_tg.c:232 initializes two embedded SSB nodes: one active, one spare.
  A live owner can therefore temporarily have no spare while its previously
  published node is being drained.
- src/lj_gc2.c:14372 returns zero without changing the active suffix when no
  replacement node can be obtained. With allow_drain enabled it first tries
  gc2_recycle_published_ssb_for_flush at line 14347. That helper is bounded by
  the existing worker-token claim and published-buffer availability; a token
  refusal or zero conversion permits zero return. It never promises that
  merely returning from flush means the owner cursor became empty.
- src/lj_gc2.c:15082 requires recovery empty, published queue/detached consumer
  empty, grey empty, and empty active cursors on the main/current/list TGs.
  Its published predicate takes two snapshots around a sequential fence and
  includes the detached consumer count. An empty MPSC head alone is not the
  predicate used by this test.
- src/lj_gc2.c:22359 starts each MARK/WEAK/SWEEP quantum by rotating only the
  proven logical owner's SSB. The real worker loop supplies its own TG. It
  cannot mutate a live VM owner's private cursor simply because that owner
  is sleeping in a C fixture. With mark_close_intent zero it drains published
  SSB, grey and recovery work; it does not initiate the owner's MARK fixpoint.
- src/lj_gc2.c:22644 performs the real fixpoint driver. It retains owner/root
  and close gates, and at line 22700 issues a flush handshake for a remaining
  private suffix. This synthetic async-drain subtest does not call that driver.
- src/lj_safepoint.c:278 also documents that a remote action cannot rotate a
  no-Lua-stack worker while traversal may append to that worker's SSB.

The fixture, TG implementation, safepoint implementation and GC2 public header
are byte-identical in all three frozen variants. Control and initial candidate
also have identical lj_gc2.c. The veto's only lj_gc2.c changes initialize the
new STOP byte and consult it during logical-stop request publication. The
SSB conversion, empty predicate, parked loop and MARK quantum are unchanged.
That comparison and the matched failures exclude a veto-only scheduler
regression as the explanation for the newly captured failure family.

The justified correction is to establish actual owner publication using the
existing bounded flush contract before asking background workers to prove
their independent drain. Keep the existing graph, worker-progress counters,
global empty predicate, and observational drain bound. Do not clear cursors,
force queue state, remove private suffixes from the empty predicate, introduce
a full owner GC drain, or extend the old observational timeout to hide this
missing publication. A correction will be a separate, reviewable artifact;
this diagnostic package does not itself change the permanent fixture.

This finding says nothing about the separate two-worker SWEEP completion
limit, the STOP/RESTART first-attachment publication races, physical string
reclamation, or overall nonblocking progress. Those remain independently
tracked. Ordinary automatic GC still has synchronous owner/root handshakes.

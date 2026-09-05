# Source alternatives after the frozen worker-two diagnosis

The smallest relevant repair is to schedule the existing SWEEP preparation
boundary from the background worker path. The missing work is the ownership
spine and pending-root frontier. Suppressing their guards, clearing rescan
counters, counting attempted work as completed work, increasing the fixture
bound or adding explicit mutator collection would not establish a repair.

This is a source proposal, not a validated patch. The exact production source
and all original failures in this package remain unchanged.

## Why the current worker cannot finish

In candidate3 `lj_gc2.c:2759`, `gc2_worker_main` runs
`gc2_worker_drain_inner`, then attempts `lj_gc2_sweep_to_idle` after a
zero-progress sweep pass. `gc2_worker_drain_inner` flushes the current worker's
private SSB before claiming `worker_active` (`lj_gc2.c:22345`), drains graph
work, and later calls arena-owner progress. It never invokes
`lj_gc2_sweep_prepare_bridge_boundary`.

The preparation boundary is called by `lj_gc2_collect_active` and
`lj_gc2_step_explicit` (`lj_gc2.c:3052,3186`), reached by automatic mutator
steps through `gc2_step_auto` (`lj_gc.c:4148`). It performs the semantic
snapshot, bounded ownership-spine pruning and final READY publication
(`lj_gc2.c:5575`). Arena progress requires that READY proof
(`lj_gc2.c:5862`); idle close independently refuses READY0
(`lj_gc2.c:6435`). These exclusions are correct and must remain.

An automatic mutator invocation competes for the same worker token while the
burst supplies substantial graph work. Its bounded attempt can return with
the bridge incomplete. `gc2_step_auto` advances the ordinary allocation
threshold before returning; the native sleep body/return performs a safepoint
poll (`lib_threading.c:2080`, `lj_safepoint.c:624`) and does not itself call the
automatic GC driver. Thus the workers can drain the graph during the sleeps
without any actor scheduling the unfinished boundary. The next allocation
burst creates more work before the next mutator boundary attempt.

The worker is not excluded by a Lua-state ownership requirement in the normal
boundary. Its claim is the existing nonwaiting `worker_active` CAS with the
inverse SMR writer check and GCSCAN/phase check (`lj_gc2.c:1494`). The observed
failure has all of those gates available. The absent scheduler call is the
coverage gap; the READY gate is not the defect.

## Preferred bounded integration scope

Add a background preparation scheduling unit outside any already-held worker
token, restricted to the existing active SWEEP phase. Reuse the normal
boundary protocol; do not borrow mainL or invoke a Lua finalizer. Keep the
current Lua-driver wrapper/behavior and all phase, native, reader, exact root,
rescan, graph and finalizer checks.

Do not simply make an unconditional scheduler call and report “one unit” as
forward progress. The current boundary returns void and self-wakes on many
incomplete returns. A worker loop can otherwise consume its own wake forever
while an unchanged root/reader/recorder condition remains. Give the internal
boundary helper an explicit outcome for real progress, incomplete ownership
or admission, and deferred work. Its result must be produced under the worker
claim, after exact root/queue holds are released. The worker outer loop must
honor a changed `deferred_epoch` and use an existing bounded backoff for an
unchanged frontier, even if its own publication changed `worker_wake`.

Real progress can include a new prepare/snapshot/READY certificate, consumed
graph work without a deferral, a root cursor advance, detached ownership
entries, or root EOF completion. The cursor comparison must remain inside the
same claimed phase/cycle; pointer changes sampled across an ownership release
are not an exact progress certificate. Zero unlinked objects alone is not a
stall: a bounded pass over post-reset live ownership entries can advance the
cursor without unlinking anything. Conversely a repeated same cursor and
unchanged certificate is not positive work just because the helper ran.

The existing prune body visits at most 256 entries (`lj_gc.c:1557,1902`), and
graph drain uses the existing 64-unit quantum. However, this is **not** an
end-to-end nonblocking/bounded-time operation: the existing boundary can wait
for a synchronous handshake, and its EOF pending-root flush walks an entire
detached chain (`lj_gc.c:5033,5111,5206`). This workload has over 240,000 such
pending nodes. Preserve that limitation explicitly in any first repair. A
strictly bounded pending-chain cursor and asynchronous handshake continuation
would be subsequent protocol changes, not something proved by adding a
worker call.

## No lost roots and no borrowed worker stack

The source permits a real worker actor to execute the normal boundary as
follows; these are proof obligations for the exact proposed implementation,
not permission to move individual operations outside their holds.

- Worker startup binds its exact TG to TLS and publishes native state with
  no `cur_L`/`thread_L` (`lj_gc2.c:2759`). `G2TG` resolves through physical actor
  identity (`lj_thr.c:1687`), so a worker-side own-SSB flush cannot resolve to
  an unrelated main TG. The normal claim excludes concurrent collector and
  SMR reclaimer mutation. It must be taken once, without nesting an existing
  drain's worker claim.
- RESET_ALLOC acts on each acknowledged TG's private allocator, and records
  `prepare_epoch` only after its exact preparation succeeds
  (`lj_safepoint.c:249`). No worker should directly rotate a running mutator's
  allocator. The boundary rechecks the phase and its existing admission gates.
- A required semantic snapshot is still the provisional `0 -> 2 -> 1`
  certificate with closed native entry and cycle revalidation
  (`lj_gc2.c:5540`). `lj_gc2_trace_sweep_roots` requests
  SCAN_ROOTS|FLUSH_SSB (`lj_gc2.c:22180`). Each TG's roots are read at its owner
  ACK or through the existing certified parked-native path; the leader does
  the once-per-snapshot global pass (`lj_safepoint.c:249,346,906`). A worker's
  lack of a Lua stack is not permission to scan an active peer's private stack.
- These bridge actions do not include FLUSHJ. Therefore the separate
  `safepoint_leader_lua_state` main-state fallback for FLUSHJ is not reached.
  Global roots use `global_State` and exact object/registry scopes. Temporary
  traversal roots and SSB rescans belong to the physical worker TG. The final
  global/reclaim suffix is flushed before handshake completion
  (`lj_safepoint.c:943,963`). Remote ACK still must not rotate another
  no-Lua-stack worker's private SSB; workers flush their own at boundaries.
- Ownership pruning remains under worker exclusion plus an ordinary SMR
  reader. Exact structural validation, root membership claim, link CAS,
  mark revalidation, allocation-generation check and detached LIVE/RETIRED
  tickets preserve the allocation and its graph (`lj_gc.c:1902`). The
  sole-main exclusive shortcut explicitly rejects background workers and a
  configured pool (`lj_gc.c:1609`); a new scheduler path must not change it.
- Pending-root flush pins the registry before atomic head detach, validates
  the entire detached chain before splice, and retains/rechecks the pending
  publication hint. Main and TLS-current heads are covered even outside the
  ordinary TG list (`lj_gc.c:5033,5170,5206`). It is an existing cross-TG
  publication protocol, not a traversal of an actively owned Lua stack.
  New objects published after allocator reset remain outside this cycle's
  old-generation reclaim set. Root EOF is not certified until final pending
  flush and hint checks settle.
- READY still requires empty graph/recovery/rescan frontiers, the completed
  semantic snapshot, closed native/recorder conditions and a final fenced
  recheck. Any retry retains its exact work and returns through claim cleanup.
  Physical arena destruction remains behind the existing READY, root-state,
  grace, reader, finalizer and allocation-lifetime protocols.
- A worker does not acquire a Lua finalizer callback state. The normal
  finalizer queue/foreign-owner veto remains effective; active STOP does not
  authorize a new IDLE cycle and the new unit must not request one. The scope
  is progress of already active SWEEP work, consistent with existing worker
  drains. The existing explicit STOP/FINPAUSE invocation-entry contract stays
  unchanged.

The already observed rootbusy constructor repair is a separate prerequisite
for broader progress claims. Its retained allocation is not this workload's
late frontier. Importing its outer automatic deferral check would not schedule
the missing worker bridge. A later combined validation must preserve both
counterexamples and must not count repeated retained-root attempts as work.

## Alternatives and acceptance

A mutator-only alternative could schedule an automatic active-cycle unit at a
fully restored VM/native return boundary. That requires its own stack,
exception, reentrancy and STOP/FINPAUSE admission proof, adds work to native
return latency and still cannot progress while every mutator remains parked.
It is less directly suited to this demonstrated worker omission.

A helpable asynchronous phase/root protocol could remove the existing
synchronous waits and whole-chain flushes. That is the route toward the user's
full lockless goal, but it requires retained epoch/cursor ownership and exact
continuation proofs. It is a larger implementation than this repair.

The first candidate must pass the unchanged worker0/2 peer0/1 automatic fixture
at the original 262,144-table/45-second bounds in normal, assert and ASan builds.
Also preserve an actual quiet/native-park control so a larger next burst cannot
mask the missing background boundary. Add a controlled pause on an existing
worker/root/reader/constructor/recorder owner and require a bounded return or
park with unchanged retained identity; release the owner and require real
cycle completion. Repeat publication/cancellation, delayed pending-root
splice, remote native-root completion, worker start/stop and finalizer cases.
Relevant existing fixtures include `t-gc2-worker-scheduler.c`,
`t-gc2-recovery.c`, `t-gc2-sweep-public-table-rescan.c`,
`t-gc2-sweep-table-coalescing.c`, `t-gc2-sweep-leaf-publication.c`,
`t-gc2-sweep-edge-lease.c`, `t-safepoint-native-root-hold.c`,
`t-safepoint-remote-root-completion.c`, `t-safepoint-local-native-duplicate.c`
and the original constructor deferral fixture. A green count alone is not a
proof: keep phase/cycle completion and exact retained-work ownership oracles.

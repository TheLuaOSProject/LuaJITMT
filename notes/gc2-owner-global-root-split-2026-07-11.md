# GC2 owner/global root split and persistent closure snapshots (2026-07-11)

Status: implemented checkpoint. The plan files were not edited. This note
records the ownership and closure protocol now present in the source, the
validation performed for this checkpoint, and work which remains P0 before a
release safety claim.

## Owner/global root boundary

`lj_gc2_scan_cycle_owner_tg_roots(g, tg)` is the only safepoint entry which
reads a live TG's private roots. It runs from that TG's acknowledgement and
scans:

- temporary-buffer backing storage and JIT temporary values;
- root-anchor blocks and slots;
- `thread_L` and `cur_L` identities and stacks when their current owner is the
  acknowledging TG;
- the TG's positive `vmstate` trace;
- tid-addressed thread `NEEDSCAN` handoffs.

`lj_gc2_scan_cycle_global_roots(g)` runs once after a full root handshake. It
scans process-global roots, registries and side structures, but never treats
registry membership or a foreign TG publication as authority to read a live
stack. Threading-state registry entries are identity roots only. Foreign live
stacks are deferred to their owner through `NEEDSCAN`.

Thread traversal now establishes both object lifetime admission and stack
ownership before validating or reading stack geometry. A live foreign owner
produces `NEEDSCAN`; a registered `TGF_DEAD` owner is retried without takeover;
only an owner tid absent under the TG-registry SMR lease can be converted to a
collector claim. The embedded main state is pinned by the global-state lifetime
and is handled separately from arena-allocated states.

The old post-ack global TG scan, foreign-stack fallback paths, and duplicate
sweep root scanner were removed. Sweep uses the same owner/global handshake.
Dead TGs free their temporary buffer before publishing `TGF_DEAD`, so the
global pass has no dead-TG private-storage exception.

## Native acknowledgement and STOP delivery

FFI callback entry now closes `in_native` with release/fence ordering before it
mutates `L->cframe`, stack state or recorder state. Final native leave uses the
same close-before-poll order. Handshake signal publication has the matching
ordering before remote-native acknowledgement.

Nonleader handshake contenders acknowledge only their exact TLS TG; the active
leader performs remote-native acknowledgement after signal publication. No-Lua
worker/native regions use `lj_safepoint_poll_tg()` and
`lj_native_leave_tg()` instead of silently discarding actions.

A remotely or TG-only acknowledged STOP request retains a synthetic poll edge
until an L-aware `lj_safepoint_checkstop()` consumes `TGF_STOPREQ_FRESH`. The
fresh bit is the one-shot consume linearization point; the sticky STOP bit can
remain set for native bookkeeping without repeatedly interrupting a caught
`pcall`. Counted request repair uses the per-TG `reqmask`, not a stale global
leader/action publication.

## Persistent MARK/WEAK/SWEEP snapshots

Per-stack dirty-epoch freshness is no longer used as a post-handshake closure
lease. A completed owner scan is a point-in-time snapshot; after its TG resumes,
phase-aware barriers and concrete SSB/grey/NEEDSCAN publications protect new
edges. Requiring every stack's dirty epoch to remain unchanged made closure
impossible under normal concurrent Lua execution.

MARK close uses `mark_root_scanned` as a `0 -> 2 -> 1` snapshot state:

- state 0 takes one full owner plus process-global root snapshot;
- state 2 serializes the in-progress snapshot and is published as state 1 only
  after phase, cycle and JIT validation;
- state 1 drains snapshot and post-snapshot work across bounded calls without
  repeating global scans;
- a real thread `NEEDSCAN` uses the new `SCAN_OWNER_ROOTS` handshake action,
  which services owner stacks without regenerating process-global function and
  prototype rescans;
- a foreign owner-local SSB uses `FLUSH_SSB` only; GC workers publish their own
  no-`cur_L` active suffix before attempting to help MARK close;
- JIT reopening invalidates the latch and requires a new full snapshot.

This fixes the measured liveness failure where every 64-item close round took a
new global snapshot, and marked root functions/prototypes regenerated more work
than that round could drain. Before the latch, the reduced active-root fixture
timed out at 30 seconds near 200% CPU with hundreds of thousands of fixpoint
rounds. After the latch, reduced and default fixtures complete normally.

WEAK close retains its existing one-snapshot `weak_root_scanned` state and now
closes solely over concrete SSB/grey, thread/table `NEEDSCAN`, new-mark, JIT,
assist, weak-clear, weak-writer and SSB-consumer producers. It does not reopen
because an ordinary stack became dirty after its acknowledged snapshot.

SWEEP uses `sweep_root_scanned` separately from `sweep_root_done`:

- `sweep_root_scanned` records the one mandatory phase-aware owner/global
  semantic snapshot;
- `sweep_root_done` remains the bounded ownership-spine pruning cursor EOF;
- the root snapshot is not repeated for every bounded spine batch;
- bridge READY is published only after root-generated SSB/grey work, thread and
  table `NEEDSCAN`, new marks, JIT activity and weak/SSB producers are closed and
  revalidated at the READY linearization point;
- post-READY owner sweep batches still defer to concrete rescue work and real
  owner handoffs, never to generic dirty-epoch freshness.

## TG registry and handshake lifetime

The root-handshake leader holds a tactical TG-registry SMR read lease across
consumed-poll clearing, leader release and the post-leader STOP rearm pass.
`gc2_hs_leader` also blocks dead-TG reclamation. This pins TG nodes while those
passes walk the registry without holding a lease across the whole handshake
(which would make retired-state reclamation self-deny). This is safe for the
current protocol, though a dedicated registry-reader domain remains desirable.

## Validation for this checkpoint

The default x86_64 Linux build completed without compiler warnings. Direct
fixtures passed against the same archive:

- `t-gc2-phase`;
- `t-gc2-traverse`;
- `t-gc2-alloc-account`;
- `t-gc2-markbits`;
- `t-arena-gcsweep`;
- `t-safepoint-handshake`, including an owner-only root action which advances
  the owner stack stamp without changing major/minor global-root scan counters;
- `t-gc2-worker-scheduler`;
- `t-ffi-ccall-native-helpers`.

`t-gc-active-thread-roots.lua` passed with churn 1 and default churn 32 under
`-joff`, and passed at default churn with the JIT enabled. The two default runs
completed together in under one second on the validation host. The focused
owner/global traversal and safepoint fixtures also cover foreign, ownerless,
stale-owner, registered-dead-owner and validation-before-ownership cases.
The M3 worker-scheduler, safepoint-handshake and active-root suite trio passed
through the repository harness. The full `m7_ffi_ccall_native`,
`m7_ffi_callback_install` and `m7_ffi_callback_runtime` harness cases also
passed after updating their returned-but-unchecked STOP fixture expectations
for the synthetic poll edge.

A target-only ASan build passed active-thread roots with JIT disabled and
enabled, the parked-worker Lua regression, the native ccall helper, callback
STOP reentry, and terminal-TG orphan drain. Leak detection was disabled for
these focused target runs; this does not replace the broad sanitizer and
platform matrix listed below.

## Remaining P0 work

This checkpoint is not a full-project or release-complete claim. At minimum:

1. Grey/SSB/weak queue growth paths which can allocate and throw must either be
   made nothrow or gain protected cleanup. A longjmp must not leak
   `worker_active`, a `0/2/1` snapshot state or MARK-close intent.
2. The generic FFI callback/native frame root publication still needs a stable,
   sequence-validated representation and a real callback-versus-remote-root
   race regression (plus TSAN/ASan stress). Private XSAVE staging is not a
   remotely readable root publication.
3. The tactical TG SMR/leader pin should eventually become a dedicated,
   explicitly tested registry-reader/reclaimer protocol.
4. Transition-straddling barrier paths should continue to be audited so every
   MARK/WEAK-to-SWEEP race either publishes mark work or routes immediately to
   sweep rescue. The mandatory SWEEP snapshot is the current safety backstop.
5. macOS/Darling, Windows/Wine, sanitizer, ABI and broad stock/JIT/FFI suites
   still need to be rerun after the larger branch is integrated.
6. Custom `lua_Alloc` remains intentionally ignored only as the separately
   documented temporary project exception requested for GC2 bring-up; restoring
   compatible allocator semantics remains required before final compatibility.

Read-only native consumed-ack boundary review, 2026-09-05

The current ordinary native return has a real dependency on the physical
handshake leader. Four deterministic Linux schedules below observe the exact
indefinite poll futex while that leader is paused. This is a demonstrated
progress limitation; the wait currently enforces necessary ownership. I found
no safe one-line removal. A request claim, local action completion, and whole
handshake completion are three different events.

No shared production, tests, notes or plan were edited. Reviewed production
files match the immutable strict runtime used by the probe byte for byte.
Shared HEAD at the final source snapshot was b4e26564542cb8bfa997a11c6a90e5e0017a2f79.
The plan changed relative to that runtime snapshot; it is included separately as
review context. manifest.json records all exact sources and hashes. Source
references below are to those copied files under reviewed-source/.

Observed schedules

All four modes use a real attached peer TG, its physical TLS actor, a real
lj_safepoint_handshake(SCAN_ROOTS|FLUSH_SSB), an actual Lua state owner claim,
and ordinary lj_native_enter_l/lj_native_leave_l. No reqmask, epoch, pending,
native-depth, scanner or state-owner protocol word is injected. Three linker
wrappers only pause real admitted calls. The probe starts a genuine MARK cycle
with lj_gc2_mark_begin and verifies its table argument is initially unmarked.
The remote scan then marks it. Automatic scheduling is stopped for deterministic
setup; this is not an automatic-GC or performance workload.

  Mode 0: pause before the real admitted remote owner-root scanner runs.
          reqmask=0, poll=1, ack=19, epoch=20, pending=2.
  Mode 1: run the real root scan successfully, then pause before its caller
          proceeds to epoch claim/application. Root is marked.
          reqmask=0, poll=1, ack=19, epoch=20, pending=2.
  Mode 3: run the real root scan and epoch claim, then pause on the real remote
          lj_gc2_flush_ssb call, before SSB/allocator application completes.
          reqmask=0, poll=1, ack=20, epoch=20, pending=2.
  Mode 2: all real per-TG actions and pending decrements have completed; pause
          the leader before the real global root pass.
          reqmask=0, poll=1, ack=20, epoch=20, pending=0.

Every observer sees in_native=0, unchanged exact state owner, returned=0, and
/proc/self/task/<owner-tid>/syscall reports SYS_futex on that exact TG's poll
address, FUTEX_WAIT_PRIVATE, expected value 1, and a null timeout. The observer
then releases the paused peer; native leave completes, its interrupted source
TValue is unchanged and still marked, and the live table survives a real Lua
collectgarbage('collect') before its value assertion. A further full collection
reaches IDLE and lua_close completes. Each mode exits 0 in about 4 ms on CPUs
0-15; these durations are test runtime only. CPU 30 was not used. alarm(20) and
an outer 25-second timeout bound each fresh process; there were no final-run
timeouts or remaining probe processes.

This is an exact wait-state oracle, not a claim that a peer was suspended for
seconds. The null futex timeout plus the source's completion predicate explain
why a indefinitely suspended leader cannot be escaped by that native return.
The wrapper before the root scanner pauses after real request ownership has
been acquired and before it uses that authority; it does not freeze an
arbitrary unused function call. Mode 3 separately exercises a request already
claimed to the current epoch while real owner-private actions remain pending.

Artifacts and reproduction

  probe.c                     final standalone probe
  probe                       strict assertion-enabled executable
  compile.json                exact compiler command and output
  mode-{0,1,2,3}.json          separate process commands, results and raw syscall
  manifest.json               source/runtime/probe SHA-256 identities
  reviewed-source/            read-only source/context snapshot

Runtime archive:
  /tmp/lj-wide-cdata-final-combined-20260905-95fv5ii6/strict/src/libluajit.a
SHA-256:
  5d404c4a30b19e7b2781caa797675b4b400e55fdba058f228ff6083664134225

Probe build: GCC -std=gnu11 -O2 -g -Wall -Wextra -Werror -mcx16, exact runtime
headers, FUNC/GC2/TAB/ARENA/TRACE test helpers, LUA_USE_ASSERT, -lm -ldl -pthread.
The exact linker wrappers are:
  lj_gc2_scan_cycle_owner_tg_roots_native_parked
  lj_gc2_scan_cycle_global_roots
  lj_gc2_flush_ssb
Run each mode as a fresh bounded process:
  timeout 25s taskset -c 0-15 ./probe 0
  timeout 25s taskset -c 0-15 ./probe 1
  timeout 25s taskset -c 0-15 ./probe 2
  timeout 25s taskset -c 0-15 ./probe 3

No runtime rebuild, sanitizer build or broad suite was run for this read-only
handoff. The probe depends on Linux /proc permission to inspect a sibling's
syscall and on x64 futex implementation; it makes no Win/macOS claim.

Preserved fixture limitations/corrections:
- initial-cleanup-assert/: first mode 0 reached the exact wait, then asserted
  immediate physical dead-TG reclaim. MARK can retain a dead TG through its
  independent grace, so that cleanup assumption was invalid. Final code keeps
  static peer storage alive through ordinary final collection/close.
- postcheck-wrapper-rearm/: first mode 2 reached the exact wait and resumed, then
  the global wrapper rearmed during the fixture's own later full GC. Final
  wrappers have a one-shot armed interval. This was not a production assertion.
- three-mode-passing/ preserves the passing pre-mode-3 fixture.
- four-mode-before-live-root-collection-check/ preserves passing wait evidence
  before adding the explicit live-table full-collection assertion.

Existing test/context review

Tests t-safepoint-handshake.c:215-278 synthesize reqmask=0/poll=1 and either clear
poll or publish a new request after 20 ms. They validate wake/gate plumbing,
but do not pause genuine remote scan ownership. The new probe supplies that
missing boundary without replacing the existing synthetic wake controls.

notes/safepoint-consumed-native-ack.md correctly describes poll as the consumed
request's completion gate. Its paragraph saying remote scans avoid frame-chain
walking is stale against current gc2_stack_scan_top: the conservative branch
calls gc2_mark_frame_chain_funcs (lj_gc2.c:7768 and :7905) before maxstack
coverage. This review uses current source, not that historical narrowing claim.
notes/safepoint-remote-ack-liveness.md correctly distinguishes remote help from
an owner about to resume: remote acknowledgements must skip an already-consumed
poll, while the mutator currently waits on it. The current leader-enter helper
also helps only its own physical TLS TG (lj_safepoint.c:790).

Two progress windows and current authority

1. Remote owner-private action is incomplete.

lj_native_leave at lj_safepoint.c:559 closes native depth and executes the
sequence fence before polling. The signaler's request-before-poll publication
and post-signal fence at :614 prevent both sides from missing the crossing.
The leader's sole remote acknowledgement branch at :685 first observes native
state and then exchanges the counted request at :327. Once it wins, the owner
cannot revoke that grant merely by storing in_native=0: it must wait before
changing stack, C frame, anchors, SSB or allocator state. The stable Lua state
owner word remains the real mutator's claim; it is not transferred to GCSCAN.

The remote root call at :364 precedes the epoch claim. The hs_epoch_ack CAS
(:199, :367) precedes safepoint_apply_tg_mode (:387). That application includes
SSB swap, allocator accounting, optional arena detach/restore, dispatch updates,
recorder abort and STOPREQ flags (:211-296). Mode 3 demonstrates current ack
with unfinished private actions. A claim loser must not invent completion on
behalf of an in-flight winner; neither reqmask=0 nor hs_epoch_ack=current is a
local action-completion receipt.

The exact remote-root scanner (lj_gc2.c:9771) obtains tactical SMR, validates
FFI native frames when applicable, and calls the full owner-root scan (:9472).
It covers more than TValue stack slots: current/thread states and all owned
NEEDSCAN handoffs, open upvalues and frame function/prototype roots, parser
LexState roots, root-anchor blocks and backing storage, tmpbuf, temporary TVs,
JIT event sessions and trace/native frame roots. State/stack identity admission
and backing leases do not grant concurrent mutation of stack geometry or
owner-private list topology. A sequence check performed after a mutable body
read cannot repair a raced plain read or storage relocation.

Consequently an owner may not resume while a paused remote scanner retains
permission to touch these mutable structures. Safe replacement is owner-only
fulfilment or immutable, retained scan evidence that the remote helper reads
without following mutable owner pointers. The dormant TG root_desc
(lj_tg.h:369) explicitly lacks complete root-writer/phase integration and is
not an existing substitute. FFI's even frame sequence is validation under the
consumed-poll lease, not independent revocable ownership.

2. Target actions are complete; leader has other work or final clear pending.

safepoint_hold_poll_until_leader (:154) holds polls for root, SSB, allocator and
trace action classes. After the pending count drains, the synchronous leader
still runs the global root pass, trace quiescence, optional FLUSHJ retirement,
metadata reclamation, final leader-local SSB publication and consumed-poll
clear, then releases hs_leader (:840-932). Mode 2 demonstrates this distinct
wait. Global pending zero is not handshake completion, and is not the proposed
exact per-TG receipt (especially across attach and changing epochs).

A durable local-completion receipt could distinguish this second window and
permit a proven subset of owners to return sooner. It cannot solve window 1.
Do not clear the poll or return merely because another target is slow without
also proving that all later leader work can coexist with this resumed target.

Trace and retirement constraints on such a partial fast path

- EXIT_TRACES/FLUSHJ must initially retain the existing full leader hold. Final
  safepoint_trace_tg_active (:710) relies on the held poll to accept an exact
  generated FFI frame with current ack, empty reqmask and live trace pin.
  lj_ffi_native_trace_remote_certify (lj_ccall.c:2140) proves that frame under
  this ownership. Native trace leave (:2322) deliberately calls native_leave
  before changing frame payload or pins (:2360). Early return could race final
  certification/retirement and is not justified by a local completion epoch.
- Preserve the existing ordinary trace-exit jit_base deadlock bypass at
  lj_safepoint.c:80, and its generated-FFI exception. It postpones the hold to
  a later safe exit boundary; it does not turn all trace returns into ordinary
  complete acknowledgements. The leader self-bypass is also distinct.
- Every synchronous handshake attempts lj_gc2_reclaim_retired after pending
  drain. Retirement timestamps use the published hs_epoch
  (lj_gc2_retire_epoch, :6695); actual free requires the completed handshake
  caller's epoch plus independent readiness/SMR/JIT exclusion (:7030, :7142).
  Trace/mcode also need LJ_FLUSH_EPOCHS and exact pins/slots; table and string
  readers have their own outer epoch pins. A local receipt must never advance a
  global completed-grace certificate or permit helper reuse of an incomplete
  epoch. New asynchronous phase work must retain these separate obligations.
- Full SCAN_ROOTS has a broader reader proof than SCAN_OWNER_ROOTS. Its global
  pass queues foreign-owned thread identities rather than traversing their
  mutable stack (:8736), which is a useful existing boundary. However
  gc2_scan_jit_roots still reads J->irbuf/irbotlim/snapbuf/snapmapbuf plainly
  (:9547-9549), and IR growth changes geometry under recorder ownership
  (lj_ir.c:76-115). The asynchronous recorder abort at :9552 does not itself
  claim the token or wait for that geometry to become immutable. This audit
  does not certify full global scan concurrent with a newly resumed recorder.
- RESET_ALLOC/RESTORE_ALLOC and mark/barrier/black-allocation transitions need
  their own phase-close proof. Keep these on the conservative path. The parent
  separately found that mark_begin retains worker_active while completing
  activation, and worker admission does not reject a live initializer token;
  releasing that token around an asynchronous request requires a persistent
  initialization-admission predicate. That is a separate change from native
  return and is not resolved by this fixture or receipt design.

Smallest safe next production step and bounded enablement target

First introduce an explicit, tail-appended per-TG action-completion epoch,
with named atomic helpers and no wait-policy change. Publish it with release
ordering only after the successful executor's final owner-private access in
safepoint_apply_tg_mode, before releasing its counted pending slot. Distinguish
'action execution finished' from 'all roots were certified': local root scan
may report durable retry/NEEDSCAN, and completion must not erase that debt.
Keep reqmask consumption and hs_epoch_ack as their current claim/count protocol.
This adds O(1) scalar publication, no new list walk, lock or allocation, and
creates reviewable evidence for window 2 without weakening window 1.

The first behavior-changing target should be an explicit, narrow action class:
SCAN_OWNER_ROOTS|FLUSH_SSB while the phase/cycle is unchanged, excluding full
global SCAN_ROOTS, all trace actions, STOPREQ combinations and allocator/phase
transitions. This is a candidate proof target, not an enabled or validated
fast path. Demonstrate no post-completion private access in the leader tail,
independent metadata grace, late SSB/root publication retained as work, and
correct epoch/poll racing before allowing its completed owner to resume. If
one wants SCAN_ROOTS first instead, the global reader authority above becomes
a required additional source audit. Do not simply weaken the existing hold mask.

Receipt/lifecycle checklist for that narrow change:
- Initialize before main/secondary TG publication (lj_tg_init_thread :385,
  attach/catch-up :547/:635); catch-up publishes completion after applying its
  private actions, not at the initial hs_epoch_ack store.
- A failed remote FFI/root certification requeues the exact counted request
  (:302/:351) and publishes no completion. A duplicate epoch claim never
  manufactures another winner's receipt or decrements pending twice.
- Tie a receipt to the exact request epoch and TG incarnation. Account for
  terminal dead-request retirement (:417) separately; death satisfies registry
  departure, not permission for a live VM to resume with unfinished work.
- Preserve same-epoch request/count ownership, late attach and request requeue.
  Request publication remains pending++ then reqmask then poll/fence/wake.
- Invalidate eligibility on a new epoch before exposing its request. Avoid
  stale receipt equality at wrap/saturation; fail closed rather than creating
  reusable zero or an ABA acknowledgement certificate.
- Owner fast-path poll clearing must not erase a newer counted request or the
  separate STOPREQ_FRESH dispatch edge. Existing clear/repair code at :104-151
  illustrates this requirement, but is not automatically a proof for a new
  early clearer. An epoch-tagged release receipt may be cleaner than letting
  arbitrary old actors write poll=0.
- Never move physical TG/free or subordinate root backing lifetime onto the
  completion bit alone. Existing registry/SMR/SSB references still apply.

A true asynchronous native-return repair then needs durable request state
whose progress is independent of the initiating C stack, and no remote body
access that depends on preventing owner return. The smallest root-side shape
is owner-only snapshot fulfilment: publish a counted request, let the exact
owner scan/publish its roots and detach the required SSB suffix, and let later
helpers consume retained evidence. An already-native peer with no such packet
keeps the GC request incomplete; it must not be remotely borrowed and then
blocked on a paused borrower. Alternatively publish an immutable retained
packet before native entry. In both variants include all the root categories
above, bounded packet/capacity failure, per-phase dirtiness and registry lifetime;
copying only L->stack is insufficient. Synchronous explicit control operations
can remain separate while automatic drivers publish/help bounded work, subject
to the parent's persistent phase-initialization proof.

Concrete acceptance oracles

1. Completion substrate: modes 0/1/3 must not expose local completion early;
   mode 2 must expose it while the leader remains paused. Add exact attach,
   duplicate, failed-scan/requeue, dead-request, next-epoch and saturation
   controls. Do not infer the receipt from the existing pending or ack words.
2. Partial return: pause a real leader after the target's eligible actions but
   before other targets/final clear, and require bounded native return plus
   ordinary owner mutation with the leader still paused. Preserve old holds
   for trace/generated-FFI, STOPREQ and allocator/phase actions. Verify retained
   roots/late SSB work and eventual full collection after release. This proves
   only window 2; do not report complete nonblocking native return.
3. Full ownership replacement: pause after the helper has actually acquired the
   root packet/read authority, require owner native return and stack/anchor/SSB
   mutation or stack relocation to finish while it remains paused, then resume
   the helper and complete the exact epoch. Retain values reachable only from
   the original snapshot and values published after it; require eventual GC
   progress/reclamation and no remote mutable-stack access. A mere pause before
   request publication or a synthetic reqmask does not test this contract.

Confidence is high for the current wait, claim-vs-completion distinction,
remote ownership requirement, and trace exclusion. The proposed receipt is a
bounded substrate design; early-return eligibility and a complete immutable
root packet are intentionally not certified or implemented by this audit.

# Local completion of the MARK owner-root handshake

This package contains an isolated candidate and deterministic functional evidence.
It changes no shared source, documentation, test or plan. The prior four-mode
handoff at `/tmp/lj-native-ack-review-20260905-lg2ml_eq` is unchanged.

The improvement is deliberately partial: after a successful per-TG action for
exact `SCAN_OWNER_ROOTS|FLUSH_SSB`, an ordinary owner can return while its leader
is still paused before the pending decrement. A scanner paused while using that
owner's private state still blocks its return. A same-epoch duplicate can also
retain the conservative old wait after the original winner has completed.

## Candidate and completion publication

Only `src/lj_safepoint.c` and its test-hook header change. There are no new TG
fields, allocation paths, phase gates, ownership transfers or wait primitives.
`candidate-source.patch` is the reviewable production plus conditional-hook patch.

The successful epoch-claim path captures a MARK cycle only for the exact action
pair. After `safepoint_apply_tg_mode` returns, it admits local completion only if
the pair is exact, the cycle is nonzero and still matches the sampled MARK phase,
and the target has neither active JIT evidence nor DEAD/STOPREQ_FRESH. Broader
masks and the duplicate-claim path keep their existing hold policy. Failed native
frame/root certification still requeues the exact request without claiming or
publishing completion.

The actual completion publication is the existing release store `poll=0` and its
futex wake, before decrementing this executor's pending slot. The owning leader
is still live. This publication changes the value tested by the futex syscall:
an owner that arrives late cannot sleep on the obsolete value 1. There is no
additional receipt whose wake could be lost while `poll` stayed 1.

The new clear immediately runs the existing `safepoint_restore_counted_poll`:
SC fence, acquire reqmask, then rearm+wake if the request is nonzero. This is
necessary even though a newer epoch cannot yet cross the clear. A leader may
have sampled old `hs_epoch_ack`, then observe reqmask zero after an owner consumes
a requeued request but before that owner publishes its claim. It may count and
publish a same-epoch duplicate. The ordinary claim-loser branch must later drop
that additional pending slot. The new clear cannot erase its dispatch edge.

If the duplicate's request/poll stores precede the completion clear, the repair
rearms it. If publication follows the repair, the publisher's own poll store
survives. A remote leader may consume a duplicate only as a claim loser, which
does not apply owner-private actions again. A genuinely newer epoch requires
the current leader to finish, and there is no completion clear after this
executor drops its pending slot. No stale global epoch or ack claim is used as
evidence that someone else's body access has finished.

The owner naturally consumes `poll=0` through the existing native-leave/poll
path; no new owner-side receipt polling is needed. Keeping `poll=1` while merely
returning from its waiter would be incorrect progress: several patched x64
JFORL/JITERL/JLOOP/JFUNCF cases back up PC and redispatch the same bytecode.

## Last private access and action failure

All source references below are to the immutable `source/src` files recorded in
the original manifest; the candidate's line numbers are shifted by its helpers.

* `lj_safepoint.c:211` applies a TG action. Root access includes parser/native
  frames, temporary buffers, anchors, cur_L/thread_L stacks and owned NEEDSCAN
  state (`lj_gc2.c:9472`). The parked remote entry holds tactical SMR and exact
  root/body scopes, then releases them before returning (`lj_gc2.c:9771-9808`).
* `lj_safepoint.c:240` then performs the requested SSB flush. The last private
  SSB operation is **not** its global published-head CAS. On success the entire
  active-to-published operation must finish installing the fresh active node,
  base/end and next cursor (`lj_gc2.c:14320`, `14369-14413`). The candidate's
  clear follows the whole call.
* `lj_safepoint.c:253` unconditionally flushes allocation accounting. Its TG
  access is an atomic local-total exchange (`lj_gc2.c:2824`); the clear follows
  that call too. There are no RESET_ALLOC/RESTORE_ALLOC/REDISPATCH/trace actions
  later in this admitted exact mask.
* If a flush cannot obtain a fresh node, it returns zero with the active work
  retained. Local completion means that the executor has stopped accessing the
  active cursor; it does not mean empty SSB. Similarly, an owner-side root scan
  can preserve a retry/NEEDSCAN obligation. Local completion never clears that
  obligation, changes root-fixpoint state, or makes the collector's close proof.
* After completion the ack path touches target-independent latency counters,
  global pending, and atomic flags/pointers as applicable. It does not access
  the mutable stack, anchors, parser, active SSB or allocator lists again.

## Every remaining handshake-tail access

This closes the initially requested single-bit OWNER_ROOTS tail audit and the
additional SSB obligations of the actual production pair. Single-bit-only early
return is not enabled: the production root-snapshot caller uses the pair.

| Remaining operation | Authority and resumed-owner interaction |
| --- | --- |
| Repeated signal/native-ack list walks (`lj_safepoint.c:614`, `685` in baseline) | Read registry next, flags, reqmask, poll, ack, in_native and trace-admission atomics. A claimed current epoch is skipped; a consumed zero request does not apply actions again. Same-epoch duplicates use the unchanged claim-loser path. |
| Full global-root pass | Absent because SCAN_ROOTS is excluded. The once-per-snapshot global JIT/current-trace scanner is not invoked by this mask. |
| Trace-quiescence/FLUSHJ tail | Absent because EXIT_TRACES and FLUSHJ are excluded. The candidate does not clear generated FFI/ordinary trace activity holds. |
| Unconditional `lj_gc2_reclaim_retired` (`lj_safepoint.c:894`, `lj_gc2.c:7142`) | MARK/WEAK/SWEEP decline IDLE readiness. If a legal phase change has made IDLE visible, the drain still requires its own metadata-exclusive SMR admission, zero-worker/recovery policy and JIT entry closure. It is not authorized by this poll clear. |
| Retired string/table/ctype/clib bodies | Owned detached records, their exact allocation/body tickets, retire epochs and current reader/header pins. No Lua stack, native frame, active anchor block or live active SSB is scanned. Table/str reader-pin scans use atomics (`lj_tab.c:2596`, `lj_str.c:381`, `417`, `473`). CLibrary cache release here does not invoke dlclose callbacks. |
| Retired trace and mcode | Require the existing opportunistic recorder token and a repeated zero-active-JIT check (`lj_gc2.c:7164`). Exact slots/body/mcode pins and retirement epochs remain authoritative. No waiting on a recorder or using its mutable scratch as root authority is introduced. |
| Detached small allocation validation | `lj_gc2_mem_registered_known_reclaim_held` (`lj_gc2.c:3678`) uses a held detached allocation ticket and atomic arena header/block state. It does not call the generic owner-list fallback at `gc2_tg_owns_small_arena` (`3415`). |
| Trace Huge extent owner lookup | `trace_arena_allocd_for_tg`/owner resolution (`lj_trace.c:2429-2448`) may read `allocd.huge`. That pointer is initialized before TG publication and is unchanged by logical detach; its transfer/clear belongs to physical TG finalization, excluded while this leader is live. HugeTab lookup has its own atomic lifetime protocol. |
| Root-pending splice during reclamation (`lj_gc.c:5168`, `5204`) | Atomic per-TG head exchange owns the detached pending chain, then CAS-splices it. Producer hint-before/after publication preserves a concurrent new suffix. It does not read a mutable VM stack or active SSB cursor. |
| Final FLUSH_SSB suffix (`lj_safepoint.c:895`) | Uses `lj_thr_get_tg_fallback(g)` of the **physical executing leader**. `lj_thr.c:1687` requires matching physical actor/TLS, including main fallback; it cannot select the newly resumed remote owner. |
| Final consumed-poll walk (`lj_safepoint.c:169`) | Reads next/flags/ack/reqmask/poll atomically. An already-cleared target is harmless. There is no concurrent next-epoch signaler before leader leave. |
| STOPREQ_FRESH rearm after leader leave (`lj_safepoint.c:183`) | Atomic flags/reqmask/cur_L/thread_L loads and poll repair; it does not dereference a Lua state. Initial FRESH targets are excluded from the new completion path. Existing counted and uncounted STOPREQ contracts remain. |

TG storage remains alive through these walks. `tg_reclaim_dead_admissible`
(`lj_tg.c:946`, rechecked under the metadata gate at `960`) requires zero live
handshake leader and pending count, in addition to the sole-main-TG/worker/MT
conditions. Logical detach may publish DEAD and clear atomic roots but cannot
physically free this TG while the current leader is live. The final walks take
an explicit tactical SMR scope before releasing that leader, preserving storage
through the post-leave rearm walk. Registered-TG fini callers first remove or
retire the registry member; bootstrap/unattached-failure cleanup cannot be this
live target. Terminal universe close requires its joined-owner lifecycle.

The metadata drain uses independent admission. `gc2_idle_reclaim_enter`
(`lj_gc2.c:7030`) closes SMR OPEN to META_EXCLUSIVE, owns JIT gate 1 to 0 (or an
exact same-thread borrowed IDLE-transition gate), fences and rechecks readers,
phase readiness and active JIT. Existing readers veto; a new reader observes the
closed gate and refuses before body access. `lj_tg_any_jit_active` only reads
atomic activity publications, not mutable frames. New post-action table/string
readers load current publication roots under their normal pin protocol. New
retirements carry the current epoch and cannot pass that same epoch's grace;
older retained readers remain represented by their epoch/header pins. Local
poll completion does not advance the global retirement epoch.

## Phase samples are a restriction, not a pin

The actual producer is `gc2_mark_root_snapshot` (`lj_gc2.c:22589-22629`): it closes
JIT entry, excludes active JIT/recorder, claims root-snapshot state 2 and requests
FLUSH_SSB plus OWNER_ROOTS only for an existing snapshot without pending thread
handoff. It still revalidates phase, cycle, gate, JIT and recorder before claiming
state 1. This change neither makes that driver asynchronous nor releases its
worker/initialization ownership.

However, `lj_safepoint_handshake` is a generic entry. A MARK/cycle sample cannot
pin phase for every caller. `lj_gc2_mark_to_weak` may publish WEAK at `4446`, and
preserve-abort may publish IDLE at `6338`, without first taking hs_leader. The
candidate does not rely on a contrary claim: even a change after the eligibility
sample is safe because all remaining tail accesses have the independent
authorities listed above.

MARK-to-WEAK preserves the ordinary active barriers/black allocation and cannot
reclaim. WEAK-to-SWEEP retains the worker/phase gate, closes entry and keeps the
sweep bridge closed through its own counted barrier/SSB action and root-drain
obligations (`lj_gc2.c:4492-4585`); those actions cannot overwrite this live
handshake epoch. Preserve-abort retains exact queue identities, publishes IDLE
under its phase/entry gate, and then performs its own counted idle barrier action
(`6250-6365`). The old active barriers/black allocation in that transient window
are conservative. The next MARK reset preserves old SSB/grey work and executes
its full activation/root protocol (`4214-4260`). Forced/ordinary close likewise
retain their separate worker/phase actions and input queues.

Resuming this owner therefore does not supply authority to reclaim, declare a
root fixed point, reset SSB storage, or skip a subsequent phase action. New
owner stack/table/root changes use the same ordinary publication barriers and
entry gates as changes immediately after the old final poll-clear walk.

An early SSB suffix is retained explicitly. Published nodes own their list link
and count; embedded nodes acquire ssb_refs before global publication and retain
it through recycling (`lj_gc2.c:14332`, `14706`). Consumers return nodes through
the atomic free list and do not replace a resumed owner's active buffer.
`lj_gc2_ssb_empty` (`15079`) checks published/detached work, recovery, grey and
each live TG's active base/next. The fixpoint (`22678-22740`) already permits
barrier-protected mutation after the root action, uses a separate FLUSH_SSB when
an active suffix remains, and rechecks all close obligations. This local poll
clear is not an SSB-empty or root-scanned certificate.

## Deterministic controls and limits

`completion-probe.c` runs each mode in a separate Linux process, alarm 20 s and
outer timeout 25 s, pinned to CPUs 0-15. It uses a genuinely attached peer/TLS
owner and a real Lua state claim, native frame, MARK cycle and root object. It
stops automatic GC for the scheduling setup, then performs real scans/barriers
and explicit full collection. These are functional schedules, not timings or
an allocation-performance result.

| Mode | Oracle |
| --- | --- |
| 0 | Leader paused after real action/clear and before pending decrement; native owner returns, changes table value, publishes a below-capacity active suffix, and full GC preserves the new value. |
| 1 | Before clear, `/proc` proves the owner is in the exact poll `FUTEX_WAIT_PRIVATE` with no timeout. After clear+wake the owner returns while leader remains paused. |
| 2 | A real subsequent FLUSH_SSB epoch is counted and polled by the closed-native owner; it cannot use the previous completion. |
| 3 | A real subsequent STOPREQ epoch retains its counted signal, one-shot FRESH poll and Lua interruption. Cleanup happens before the catchable interruption. |
| 4 | A new physical TG attaches while leader is paused, adopts MARK/black state and self-catches the current epoch, then logically detaches DEAD with no request/poll left. |
| 5 | Test wrapper forces remote scan refusal. The actual owner takes the requeued action and performs the real root scan. Same-epoch duplicate publication is allowed in its reported signal count. |
| 6 | A real separate preserve-abort publishes IDLE while the old leader is paused; the resumed owner mutates and publishes work there. The new idle barrier epoch remains counted, the owner services it, and GC preserves the result. |
| 7 | Pauses a consumed owner request before its epoch claim and the real leader after it counts a duplicate but before publication. That duplicate is released just before the winner clears poll. The post-clear repair must preserve its exact nonzero counted edge; three publications settle to pending zero. |
| 8 | Same duplicate as mode 7, then pause the genuine leader at reclaim tail with pending zero. The claim-loser owner still enters the exact poll futex although its winning action completed. This documents the remaining conservative wait. |

The exact no-repair source control deletes only the added counted-poll repair
call. Mode 7 aborts at the assertion `reqmask == 0 || poll != 0`; it does not
merely time out. The rejected receipt-only draft is separately archived and was
never built or executed: it had a check/receipt+wake/futex-wait lost-wake window
on the unchanged poll value. Neither is a claim about an observed production
failure in the original runtime.

The old four full-SCAN_ROOTS modes are compiled unchanged against this candidate
and retain their actual kernel waits, including before/during root scan,
after claim before SSB flush and after target actions before global roots.
The canonical safepoint fixture covers late attach, new epochs, dead counted
slots, trace/allocator actions, nested native entry and STOPREQ/rearm paths.

The canonical fixture initially failed on **both** immutable baseline and
candidate at its setup assumption that a full collection leaves embedded SSB
node 0 active. The private `canonical-setup.patch` checks either embedded active
node, a distinct available free node, exact owner bindings, free-list termination
and the active node's correct cursor storage. It preserves all subsequent
counts/SSB capacity/handshake assertions. No shared test was changed.

Final strict, ASan+LSan, unchanged prior-hold and canonical results are in the
final validation directories and manifest. Initial harness assertion failures
and earlier source/build results are preserved separately; they are not silently
counted as passing. ASan uses `detect_leaks=1:abort_on_error=1`; generators are
uninstrumented. No stock suite, benchmark, TSan or release claim is made here.

The candidate does not remove the owner-private scanner wait or the duplicate
claim-loser tail wait. A future persistent completion receipt could improve the
latter, but must also change a futex predicate or otherwise close the lost-wake
window. The present change uses the existing executor-owned completion word and
keeps that larger proof out of scope.

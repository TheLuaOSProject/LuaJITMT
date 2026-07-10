# Token-owned JIT retirement, explicit trace exits, and VM events (2026-07-10)

This note records implementation decisions that deliberately refine or diverge
from `plan/08_jit_compiler.md`. The plan itself is unchanged.

## Trace retirement linearization

A collector cannot safely claim a trace's retirement epoch independently of
the recorder token. The recorder and assembler validate a target and later
publish a direct machine-code link. An epoch-only GC claim between those two
operations would let newly published code target an already retired body.

The implemented protocol is therefore token-owned and single-writer:

1. A GC/root-prune caller makes one nonwaiting recorder-token attempt.
2. On collision it requests an asynchronous recorder abort and returns without
   changing the trace epoch, retire list, public slot, or semantic root.
3. The token owner dual-preserves the body, release-publishes an encoded
   `epoch + 1` claim (`0` remains live), and publishes the embedded tagged
   retire-list node before any slot/root disconnection.
4. The bounded collector pass may splice the root only after that operation
   reports that the exact body is discoverable through the retire list or its
   exact public slot.
5. Runtime reclaim exchanges the list only while holding the zero-reader SMR
   gate and the recorder token. It clears an exact slot identity, retries an
   optional debugger unregister, tears down the exit table, release-publishes
   `gct = 0`, and only then returns the allocation to its allocator.

The claim transaction also disconnects executable entry edges before releasing
the recorder token. A root claim restores its patched bytecode and resets its
root-family exit tables. A side claim resets the exact parent exit target and
removes the side from its root chain before clearing its semantic links. This is
required even though `retire_epoch` gates interpreter entry: a parent exit table
is a persistent native pointer and cannot be aged out by merely completing more
SMR epochs. Reclaim additionally scans for a still-runnable trace whose terminal
`link` names the target; such a target is retained until that source is flushed
or retired, since the target cannot retarget an arbitrary inbound terminal jump.

Both IDLE-handshake and SWEEP-owner JIT reclaim now decline immediately while
any TG publishes compiled entry/exit activity, and recheck after acquiring the
recorder token. This covers a safepoint leader that must skip waiting on its own
`jit_base`: ordinary retired tables may still drain, but trace slots, bodies and
mcode remain intact until the leader has completed exit restoration.

The list uses low tags in `GCtrace.retired_next`: `1` is unlinked and bit `2`
means listed. All runtime insertion, detach, and requeue operations hold the
recorder token. This removes the former containment scan and same-node helping
problem, including duplicate insertion and cyclic-list failure modes.

`LJ_FLUSH_EPOCHS` remains finite. A retired public trace number names the exact
body for stale native exits and patched-bytecode readers until the epoch margin
and the outer zero-reader gate both complete. Only then can the slot be reused.

## Allocator quarantine ownership

Trace retirement owns compact payload and exit-table destruction. Arena
quarantine must not interpret retire-list publication or slot removal as
physical completion.

For small arenas, a quarantined trace remains `RETIRED` until the trace
reclaimer stores `gct = 0` and the allocator transfers the cell to `FREEING`.
For huge arenas, allocator free suppresses delete/unmap while sweep owns a
`RETIRED`/`FREEING` entry; the quarantine owner performs the eventual exact
hugetab delete and unmap after observing physical completion.

The sweep-owner reclaimer runs after bounded root detachment. Consequently the
trace reclaimer skips `lj_gc_unlink_root_obj()` in `LJ_GC2_SWEEP`; doing another
full ownership-spine walk there would destroy the bounded quarantine property.
The ordinary IDLE fallback still performs that compatibility unlink for a
trace retired by `jit.flush()` before a GC sweep has detached it. Replacing
that remaining potentially unbounded IDLE walk with an exact/bounded root
detach publication is follow-up nonblocking debt.

Shutdown is exceptional but exact. It may wait for the recorder token and the
process-global GDB descriptor, because allocator teardown cannot leave either a
public trace slot or debugger entry pointing into freed universe storage.

## Machine-code retirement

Every executable area now receives a writable `MCodeRetire` sidecar while area
creation is already a fallible recorder transaction. Active sidecars and the
executable area chain have identical order. Full flush exchanges the preowned
active list, epoch-stamps it, preserves/publishes it on the retired list, and
detaches the executable chain without allocating or throwing.

The same SMR gate and recorder-token transaction drains trace bodies before
machine-code areas. Retired trace bodies are scanned for RX and RW-alias
references; an area is unmapped only after its epoch matures and no preserved
body references it. Details and allocation-failure coverage are also recorded
in `notes/jit-mcode-preowned-retirement-2026-07-10.md`.

## Explicit x64 trace-exit state

The x64 exit stub passes its initiating `lua_State *`, parent trace number, and
exit number directly to `lj_trace_exit()`. System V uses five arguments. Win64
packs the two public 16-bit IDs into the fourth register argument, avoiding
writes to shared `J->L`, `J->parent`, and `J->exitno`.

Error unwind and exit restore use the currently executing TG obtained through
`G2TG(G(L))`, not the coroutine's migratable `tg_hint`. This applies both to the
TG-local exit code and to the early GC-defer `jit_base = NULL` quiescence
publication. `errno` and Windows `LastError` are restored before every exit
return. Snapshot restore also consumes the explicit exit descriptor on every
x64 ABI, including Win64; retaining the legacy `lj_snap_restore()` call there
would have re-read stale or cleared recorder fields despite the corrected exit
stub.

## VM-event callback ownership

The plan proposed token-holder-only VM-event sends. That loses normal TEXIT and
initiating-state events under lockless multi-TG execution, so the implementation
uses a narrower, nonwaiting callback domain instead:

- Handler lookup and arguments use the initiating `lua_State`, never the shared
  compatibility `vmthread` stack.
- Prepare reserves stack space before acquiring a handler. The registry load
  and the single replacement stack-root store are then enclosed by a one-shot
  GC2 SMR reader claim; `lj_state_stack_pubtv()` release-publishes and
  marks/rescues that exact function slot before the reader leaves. Thus a
  concurrent detach may remove the source registry root without exposing an
  unrooted C-local handler across stack growth or the load-to-root handoff. A
  collision with active reclamation drops the observational event immediately
  and keeps its cache bit retryable instead of waiting for the reclaimer.
- A process-wide exact-TG `vmevent_owner` CAS serializes only protected callback
  execution. A collision drops the observational event and restores the exact
  pre-prepare stack top; it never waits for a peer callback.
- VM events enter only when debug, profile, GC, and VM-event hook ownership is
  clear. Leave clears this callback's `HOOK_VMEVENT` delta and preserves
  concurrent hook activity instead of restoring a stale hookmask snapshot.
- A callback that does not own the recorder token never writes `J->L`. A token
  owner restores only the exact pointer it sampled while ownership remained
  unchanged.
- Callbacks no longer zero and restore the global VM-event cache. The owner gate
  already suppresses recursion, and a temporary zero could erase a concurrent
  `jit.attach()` invalidation. Missing-handler caching revalidates once after a
  clear so an attach racing lookup cannot permanently suppress the new handler.

Collision drops are safe and bounded, but they are not lossless event delivery.
A lossless per-TG queue with payload snapshots would be a larger semantic
extension if every instrumentation event must eventually run; the current
contract treats concurrent VM-event callbacks as racy observation.

Full sticky-MT flushes keep the recorder token through the initiating state's
public TRACE `"flush"` callback. Releasing it first could let a peer recorder
publish `J->L` and then have that owner overwritten by callback cleanup.

## Optional GDB JIT metadata

The GDB JIT descriptor is process-global even across independent Lua
universes. Runtime registration and unregister are now try-only. A registration
collision discards that trace's optional debugger object; an unregister
collision retains the trace and retries at a later grace pass. Neither path
parks the recorder or SMR reclaimer behind another universe. VM close is the
sole blocking exception and completes unregister before allocator destruction.

## Remaining nonblocking/performance debt

The correctness changes above deliberately retain a few cold-path operations
whose worst-case cost is not yet bounded independently of trace population:

- `trace_flushscope_mark_deps()` computes dependency closure by repeatedly
  scanning the full trace vector. In a deep dependency chain this is
  `O(maxtrace * dependency depth)` and can become quadratic. Replace it with an
  indexed dependency work queue or reverse-edge structure whose publication
  and retirement ownership are exact.
- `trace_retired_slot_release()` has a whole-slot scan as an invariant-recovery
  fallback. Normal reclaim has the exact reservation, but the fallback is still
  `O(maxtrace)`; eliminate it once every reservation transition carries a
  validated slot identity.
- Retired-target reclaim performs a cold whole-vector scan for runnable terminal
  links. It is safe and nonwaiting, but still `O(maxtrace)` per mature target.
  Maintain an exact reverse-link index under the recorder token to make this
  validation bounded independently of trace population.
- Runtime mcode reclaim calls the platform `__deregister_frame()` before
  unmapping an area. The LuaJIT-side descriptor protocol is try-only, but the
  system unwinder may serialize internally. Either prove the supported
  unwinders' deregistration path nonwaiting or move this work to a dedicated
  deferred reclaimer while preserving the mcode lifetime proof.
- The ordinary-IDLE compatibility root-spine unlink remains potentially
  unbounded, as described above, and callback collision currently drops an
  observational VM event. Exact bounded root detach and a lossless per-TG event
  queue remain separate follow-up items.

## Focused validation

The changed trace, VM-event, GDB JIT, error, and fixture translation units pass
warnings-as-errors builds with `LUA_USE_ASSERT` and `LUA_USE_APICHECK` on:

- Linux x86-64 with GCC;
- Windows x64 with MinGW;
- macOS x86-64 with osxcross Clang.

Both System V and Win64 DynASM generation succeed. Focused fixture compilation
covers recorder-token/VM-event ownership, trace retirement, trace-vector reuse,
preowned mcode retirement, dormant XSAVE snapshot instrumentation, forced
VM-event prepare-time stack growth against concurrent handler detach/collection,
and repeated guarded side exits using explicit exit state. Full runtime, Wine,
and Darling results belong in the integrated validation record after the
concurrent GC quarantine state machine is coherent.

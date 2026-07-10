# Sticky-MT trace-slot reuse

## Problem

`mt_active` is intentionally sticky after the first secondary Lua thread has
run. Trace retirement used that latch as a process-lifetime reason to keep every
retired `GCtrace` body in its public trace-vector slot. The body was repeatedly
requeued even after `LJ_FLUSH_EPOCHS` completed safepoint generations.

This made every old trace number permanently unavailable. In a one-thread gap
after a worker generation, repeated `jit.flush()` plus re-recording eventually
filled `maxtrace`; all later recording attempts failed even though no live trace
used the namespace. Retired bodies, exit tables, KGC operands, and prototype
payloads also remained live until VM shutdown.

## Reuse rule

A retired public slot is an exact-body exit-restore name for a finite grace
period, not a process-lifetime name:

1. Retirement clears `T->traceno`, stores the old slot number in the retired
   body's private `nextroot` field, and leaves that exact body in the public
   slot. New VM/recorder entry is rejected by the cleared trace number and
   non-zero `retire_epoch`.
2. The body and its snapshot/KGC/prototype payload remain preserved for
   `LJ_FLUSH_EPOCHS` completed safepoint generations. Trace-exit and unwind
   readers additionally hold the existing GC2 SMR read section.
3. Retired reclaim only mutates trace slots while it owns, or can
   opportunistically acquire, the JIT token. It never waits for a recorder while
   the outer SMR reclaimer is published.
4. Once the epoch margin has elapsed and the outer SMR gate observes zero
   readers, reclaim clears the slot only if it still names the exact retired
   body, updates `freetrace`, unlinks the old root-spine body, and frees it. A new
   trace may then publish the same number without a stale reclaimer clearing the
   replacement.

The safepoint boundary is the generation proof. A thread holding a trace body or
machine-code exit either acknowledges after leaving that dependency or keeps the
handshake pending through its published `jit_base`; the two-generation margin
then covers stale bytecode-entry and exit-restore readers. Reused trace entry
still validates trace number, `retire_epoch`, pending state, and start PC.

## One-thread gaps

Before first MT activation, one-TG `jit.flush()` retains the stock direct path.
After `mt_active` becomes sticky, a one-TG full flush goes through the safepoint
leader path so it advances the universal retirement epoch. Without that step,
all direct flushes in the gap would stamp the same epoch and no reserved slot
could mature, even with finite reclaim rules.

The leader action itself remains eventless because an arbitrary leader does not
own the initiating VM-event stack. Once the handshake returns, the initiating
`lua_State` emits the normal TRACE `"flush"` event while it still owns the
recorder token. This path calls `lj_vmevent_prepare()`/`lj_vmevent_call()` on
that state directly instead of using the shared `vmthread`. Releasing the token
first allowed a peer recorder to start and then have `J->L` overwritten by the
flush event; using `vmthread` could also race a peer TEXIT event. The serialized
initiator-state callback keeps the public event contract without either race.

## Trace-exit GC handshake cycle

The threaded flush stress exposed a post-ack deadlock between two trace exits.
One TG led an `EXIT_TRACES` mark-start handshake and reached trace quiescence
with `hs_pending == 0`. A peer had acknowledged the same epoch, but was blocked
entering a nested GC handshake from `lj_trace_exit()` while its `jit_base`
remained published. The leader could not retire trace state until that base was
cleared; the peer could not return through `vm_exit_interp` to clear it until the
leader released handshake ownership.

Snapshot restore, the TEXIT callback, its SMR read section, and saved-PC
publication are already complete before the GC-defer branch in
`lj_trace_exit()`. That branch now clears the current TG's `jit_base` with a
release store before fixpoint work or `lj_gc_step()`. It no longer depends on
the exiting trace body or mcode there; the later BC_JLOOP lookup opens a fresh
SMR read section and revalidates the target. The assembly clear on return is
idempotent. Trace exits that may record a hot side trace keep the original
protection.

## Recorder-owner teardown

A short-lived worker can take a BC_FUNCF hot edge immediately before returning
to the threading C boundary. If a peer asynchronously aborts that recorder, its
active bit is cleared, but normal dispatch may not run again to finish abort
cleanup. Detaching the worker then strands the global JIT token under a dead TG
id, so later `jit.flush()` or `jit.off()` spins forever.

Spawned-worker cleanup, attached-thread error cleanup, and public foreign-thread
detach now cancel recorder state before releasing the Lua state or TG. The
teardown path first proves that TG owns the token, then restores any pending
bytecode patch, aborts mcode generation, frees unpublished final/exit metadata,
clears the unpublished slot and recorder scratch state, publishes INTERP/IDLE,
updates dispatch, and releases the token. It deliberately does not use normal
`trace_abort()`: after `lua_pcall()` unwinds, penalty/down-recursion and TRACE
abort-event logic must not inspect the stale recorded PC or walk the new frame
chain.

## Regression coverage

`m6_jit_mt_activation_flush` now sets `maxtrace=8`, records a pre-MT loop,
activates and joins a worker, then performs 24 record/flush generations. Every
generation must form a live trace. The old process-lifetime reservation fails
deterministically at churn round 9.

`t-jit-flush-thread-stress.lua` also treats worker preheat as a bounded retry:
two workers can legitimately contend for the single recorder, so one preheat
pass is not proof that the worker generation cannot publish a trace. Its
short-lived churn and final full flush are the regression gate for a recorder
token surviving its owner TG.

Focused validation targets:

- `m5_jit_trace_publish`
- `m6_jit_flush_hs`
- `m6_jit_mt_activation_flush`
- `m6_jit_flush_thread_stress`
- `m6_jit_util_flush_race`
- assertion-build `t-gc2-traverse`

The change does not alter public LuaJIT API or ABI layout. It does add a
safepoint handshake to `jit.flush()` in a one-TG gap after MT activation; full
flush is a cold control operation, and bounded namespace/memory use is preferred
over retaining retired trace bodies for the lifetime of the VM.

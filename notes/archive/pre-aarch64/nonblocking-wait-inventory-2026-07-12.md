# Core nonblocking wait inventory (2026-07-12)

Status: open implementation inventory.  This is not a completion claim and it
does not modify `plan/`.

The lifetime/GC2 correctness work exposed an important distinction which the
source must enforce mechanically: an operation can be lock-free in the broad
system-progress sense while still making one Lua mutator wait or yield behind a
peer.  The requested runtime contract is stricter for ordinary VM, JIT, GC and
FFI work.  Those paths must use bounded try/restart, publish asynchronous work,
or conservatively retain and return.  They must not call a futex, sleep, or
cooperative-yield loop merely because another core owner is active.

The audit command was:

```sh
rg -n 'lj_thr_retry_yield|gc2_peer_wait_no_l|lj_gc2_smr_read_enter|lj_tab_wait(_l|_no_l)?|lj_gc2_.*wait|futex.*wait|cond.*wait' src --glob '*.[ch]'
```

The expanded source audit counted the following current textual groups
(definitions and declarations included, and groups overlap):

- 42 `lj_gc2_smr_read_enter` sites, of which 40 are callers;
- 133 table-wait helper sites: 69 `wait_no_l`, 13 `wait_l`, and 51
  `store_wait_l`;
- 34 GC2 peer-wait sites and 27 `lj_thr_retry_yield` sites;
- 44 futex wait/helper expressions;
- 16 `lj_jit_token_acquire_wait` sites, including 13 callers; and
- 10 join expressions.

The blocking SMR API currently loops through `lj_thr_retry_yield()` until an
opportunistic reclaimer leaves.  These calls occur on ordinary debug, trace,
assembler, recorder, bytecode writer, safepoint, table, GC, GDB-JIT and
`jit.util` paths.  They are forbidden core waits, not harmless cold paths.

## Allowed blocking boundary

Waiting remains natural only where the Lua program explicitly selected a
blocking operation, or at the two temporarily exempt global-definition
boundaries:

- `thread:join`, blocking mutex acquisition, blocking channel send/receive,
  and explicit profiler/debugger stop/join operations;
- module loading through `require`/loader cache arbitration;
- FFI declaration/type mutation through `ffi.cdef` and its parser token;
- terminal process teardown after every mutator/worker has been joined.

Even these paths must publish native-time/safepoint state and remain memory
safe.  Their presence must not justify a wait in an ordinary table lookup,
allocation, GC barrier, trace query, FFI call, callback, or collector phase.

## Required conversions

1. Retire `lj_gc2_smr_read_enter()` from core call sites.  Each caller must use
   `lj_gc2_smr_read_try()` and either restart from an owned root, requeue exact
   work, or skip an optional early-preservation optimization.  Once the last
   core caller is gone, remove the looping API rather than leaving an attractive
   blocking primitive.
2. Replace table `KEYLOCK`, `FORWARD`, resize and publication waits with
   versioned descriptors and helping/bounded restart.  A CAS loser must not
   wait for the winning mutator.  FINREG claims need a table-wide descriptor so
   resize can defer without polling a claimed slot.
3. Replace GC2 `gc2_peer_wait_no_l()` and worker/finalizer handoff waits on
   mutator paths with durable queue/cursor work.  Worker startup/shutdown waits
   are lifecycle operations, not permission to block a normal assist.
4. Audit JIT token/trace-vector/mcode readers so failed tactical SMR admission
   aborts recording or retries from trace number plus generation; it must never
   yield under the recorder token.
5. Audit callback and ordinary FFI paths separately.  Callback carrier
   contention must allocate/lease another carrier rather than wait, and generic
   traced calls remain gated until their exact native frame and trace/mcode pin
   are complete.
6. Replace synchronous safepoint leader/pending/consumed-ACK waits with
   asynchronous action epochs.  Owners self-ack at polls/native leave; a leader
   must publish entry gates and defer destructive work instead of stopping an
   unrelated mutator on `tg->poll` or a handshake futex.
7. Publish string table and canonicalization topology as immutable RCU
   generations.  Interning and resize must never wait for the resize owner or
   for old readers; old headers and bodies retire by exact epoch.
8. Make `lj_state_tryclaim()` actually nonwaiting when it observes
   `LJ_THREAD_GCSCAN`.  Ordinary state claims must use a try/fail path outside
   explicit lifecycle operations because the scanner already knows how to back
   out and requeue.
9. Separate the permitted `ffi.cdef` parser writer from ordinary CType readers.
   Calls, callbacks, conversions, arithmetic, cdata indexing and layout queries
   need immutable published CType generations, not `lj_ctype_parse_wait()`.
10. Replace a callback slot's single hidden foreign-thread carrier with a
    lock-free carrier pool.  A TLS-less callback must lease or allocate another
    carrier instead of reaching `lj_threading_attach_wait()` and the state-owner
    futex.
11. Bound channel spin-only safepoint deferral.  `chan_wait()` and the
    `chan_spin_changed()` fast return in timed waits currently check fresh
    STOPREQ but can avoid both native entry and an ordinary handshake ACK while
    continuous futex churn keeps winning the spin race.  Timed waits are
    deadline-bounded; untimed waits need an explicit bounded retry counter or
    ordinary safepoint poll so lock-free channel progress cannot indefinitely
    delay a GC root epoch.

Additional P0 runtime gates include the blocking JIT token acquisition used by
trace flush/configuration, dispatch-update losers followed by synchronous
redispatch handshakes, parser/serializer dictionary waits outside the allowed
`require`/`cdef` boundaries, and GC2 peer waits reachable from automatic GC.
Terminal joins, explicit blocking threading APIs, `require`, and `ffi.cdef`
remain the only relevant exceptions.

The current tranche has already converted threading root publication and two
table storage/keyed-CAS SMR sites to nonwaiting `try` behavior.  The inventory
is complete only when a source gate proves there are no forbidden core calls,
and runtime contention tests prove bounded completion on Linux, Wine/Windows,
and Darling/macOS.  On 2026-07-12 this exhaustive evidence gate was explicitly
moved from the functional GC2+JIT `b1.2.0` beta to `b1.2.1`; see
`b1.2.0-release-gate-2026-07-12.md`.  A wait that causes an ordinary GC2/JIT
hang remains a `b1.2.0` correctness blocker even though complete wait removal
does not.

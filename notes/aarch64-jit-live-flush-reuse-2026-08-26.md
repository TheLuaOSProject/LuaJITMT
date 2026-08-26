# ARM64 JIT live FLUSHJ, grace reclamation and trace-slot reuse

Date: 2026-08-26

## Checkpoint

This checkpoint closes the first complete native lifecycle loop for the
constrained macOS ARM64 JIT root:

1. enter an already-admitted integer `BC_LOOP` root;
2. publish a real `LJ_GC2_HS_EXIT_TRACES|LJ_GC2_HS_FLUSHJ` handshake only
   after root-entry admission has finished;
3. leave generated code through the loop's native `IR_XPOLL` at snapshot 5;
4. retire the exact trace body and its executable mcode without re-entering or
   re-recording the restored interpreter tail;
5. retain raw trace slot 1 through the required SMR grace generations;
6. reclaim the trace body and mcode through ordinary completed handshakes and
   the normal IDLE reclaim gate; and
7. publish and execute a different strict integer root in the reclaimed public
   trace slot 1.

The same test runs as ordinary ARM64 and as ARM64e with BTI and authenticated
trace entry/exit enabled.

## Files

- `tests/t-arm64-jit-live-flush-reuse.c`
  - native concurrency/lifecycle fixture;
  - uses only existing production and test-hook APIs;
  - contains no manual safepoint publication and no direct trace/mcode reclaim.
- `tools/ci/arm64_jit_live_flush_reuse_contract.sh`
  - serializes architecture-specific rebuilds;
  - checks the ARM64 admission policy and ordering of the two test boundaries;
  - builds the fixture with warnings as errors;
  - runs three ordinary ARM64 iterations and one ARM64e/BTI iteration;
  - restores the checkout to the ordinary experimental ARM64 build on success
    and on failure.

No production source change was needed. The post-admission root-entry pause and
the safepoint reqmask-before-poll pause added by earlier checkpoints provide the
necessary deterministic observation points.

## Why the FLUSHJ leader is a registered peer

A TLS-less raw pthread is suitable for the observation-only coordinator, but
it is not a valid trace-flush owner. Full flush first acquires the recorder
token, whose owner identity is derived from a registered TG. A raw pthread has
no TG/tid and cannot satisfy that ownership protocol.

The fixture therefore creates a rooted secondary `lua_State` and performs a
temporary attach/detach before recording. This has two purposes:

- it activates the sticky `mt_active` latch while there are no traces, so a
  later retirement keeps the stale public slot reserved; and
- it proves the state can be reattached as a real peer TG for the live flush.

After the original trace is recorded, that same rooted state is reattached on
a pthread. The attached peer calls `lj_trace_flushall_hs_noevent()`, owns the
recorder token, becomes the safepoint leader, and detaches only after its own
epoch acknowledgement, reqmask, poll and `jit_base` are all clean. The main
thread drains the dead peer TG before beginning grace reclamation.

## Deterministic publication schedule

Three roles participate:

- **main TG**: calls the already-compiled strict loop;
- **registered peer TG**: requests the real full JIT flush;
- **raw coordinator pthread**: observes publications but owns no VM state.

The order is:

1. The root-entry helper publishes `main_tg->jit_base`, validates the trace
   slot/body/mcode twice, performs its final request and bytecode checks, then
   pauses at `LJ_TRACE_ROOT_ENTRY_PAUSE_POSTADMISSION`.
2. The registered peer begins `EXIT_TRACES|FLUSHJ`, advances the real safepoint
   epoch, publishes the main TG's reqmask, and pauses before publishing poll.
3. The coordinator requires the exact peer tid as leader, the exact action
   mask, the new epoch, the old main-TG acknowledgement, non-null `jit_base`,
   matching reqmask, and `poll == 0`.
4. The coordinator releases the signal hook, observes `poll == 1` with the
   same reqmask and live `jit_base`, and only then releases root entry.
5. Generated ARM64 code observes the request at `IR_XPOLL` and exits through
   parent trace 1, snapshot 5. `vm_exit_interp` clears `jit_base` and
   acknowledges the real request.
6. The peer waits for trace quiescence, performs the eventless full flush,
   completes its opportunistic reclaim pass, clears consumed polls, and leaves
   safepoint leadership.
7. The main call finishes in the interpreter with the exact result `210`.

The fixture requires exactly one root-entry publication, zero admission
cleanups, and exactly one native exit at parent 1/snapshot 5. It also requires
the safepoint hook to observe one resumed poll store, one consumed-poll clear,
and one clean-before-leader-leave event. This distinguishes native XPOLL from a
pre-entry rejection or a later interpreter-only poll.

The hotloop threshold is raised after recording and before the live flush. The
restored interpreter tail therefore cannot immediately record a replacement
while the old body is still inside its grace window.

## Immediate retirement state

Let `E` be the epoch completed by live FLUSHJ.

Immediately after the peer has detached and its dead TG is drained, the fixture
requires:

- the original loop bytecode is restored from `BC_JLOOP` to its exact saved
  `BC_LOOP` instruction;
- the original prototype publishes no trace number;
- raw trace slot 1 still names the exact old body;
- the body is not runnable: `traceno == 0`;
- `nextroot == 1` is private stale-slot reservation metadata;
- live routing (`link`, `nextside`) is disconnected and native pin count is 0;
- the encoded trace retirement stamp is `E + 1`, which decodes to `E`;
- the old body is the sole trace-retire-list node;
- active mcode ownership and current-area fields are empty;
- the exact old mcode sidecar is on the retired list with raw epoch `E`;
- total mapped-mcode accounting is retained; and
- `J->freetrace == 0`, so recording cannot reuse the reserved name early.

Successful assembly itself creates one separate unpublished scratch body. The
fixture characterizes it before the live race as exactly one nonsemantic node:
no trace number, slot reservation, prototype, start PC, mcode or mcode size.
The hotcount-reset handshake and live FLUSHJ provide its two real grace
generations, so the FLUSHJ reclaim pass removes it. The fixture proves the
scratch address is absent from the post-flush retire list rather than assuming
that the baseline list started empty.

## Real grace generations and normal reclamation

`LJ_FLUSH_EPOCHS` is 2. Trace bodies encode the retirement generation as
`retire_epoch + 1`; mcode sidecars store the generation directly.

| Completed epoch | Trace slot 1 | Retired mcode | Expected result |
|---|---|---|---|
| `E` | exact old body, non-runnable | exact old area | retained |
| `E + 1` | exact old body, non-runnable | exact old area | still too young |
| `E + 2` | null | empty list | trace body freed, area unmapped |

Both grace steps are real sole-main-TG
`lj_gc2_handshake(..., LJ_GC2_HS_REDISPATCH)` calls. Before each step, the
fixture requires the complete normal-IDLE reclaim preflight:

- legacy GC pause state and GC2 IDLE phase;
- no cycle leader, GC worker, assist, weak-drain or weak-write owner;
- no activation veto;
- open JIT phase gate;
- open SMR mode and zero readers;
- no active JIT TG, no recorder token owner, and idle recorder state; and
- no recovery failure.

The fixture does not call `lj_trace_reclaim_retired()`,
`lj_mcode_reclaim_retired()`, or the test-only IDLE reclaim-entry API. Each real
handshake invokes the production `lj_gc2_reclaim_retired()` path. At `E + 1`,
the trace/mcode reclaim epoch memo fields advance but both resources remain. At
`E + 2`, trace reclamation clears the exact public slot before mcode reclamation
unmaps the now-unreferenced area in the same token transaction.

The final exact counters are:

- trace slot release calls: 1;
- trace slot release clears: 1;
- last released trace number: 1;
- trace and mcode retire lists: empty;
- total mapped mcode bytes: 0; and
- `J->freetrace`: 1.

## Trace-number reuse and native replacement

Recording is reopened at `hotloop=1` only after `E + 2`. The second function
has a distinct `GCproto` but the same admitted strict integer loop shape. Its
first recording call must produce:

- exactly one `trace_findfree()` call;
- exactly one reuse, not a trace-vector grow;
- `last_findfree == 1`;
- runnable trace 1 whose `startpt` is the new prototype;
- zero retirement stamp and zero native pins;
- a new active mcode area and no retired mcode; and
- the old prototype still publishing trace number 0.

Counters are then reset independently of the recorded body and the new function
is called again. The replacement must enter native trace 1 and take the ordinary
loop-condition exit at parent 1/snapshot 8 with exact result `210`. Pointer
inequality with the old body is intentionally not asserted: the allocator is
allowed to reuse the same virtual address after the old body's lifetime has
ended. Identity is instead established by the public slot, runnable metadata,
and the distinct new prototype.

## Validation

The contract completed on the native Apple Silicon host with:

- 10 repeated ordinary ARM64 fixture runs during fixture stabilization;
- 3 clean ordinary ARM64 runs from the final contract;
- 1 clean ARM64e/BTI run from the final contract; and
- fixture compilation under `-Wall -Wextra -Werror` for both architectures.

The only build warnings were the already-known production warnings:

- `lj_trace.c`: `szmcode` set but unused;
- `lj_ccall.c`: `ccall_rawchild_wait` unused.

The contract restores the shared build to ordinary experimental ARM64 after
the authenticated run.

## Remaining limits

This is a complete lifecycle proof for the currently admitted single integer
`BC_LOOP` root, not a general ARM64 JIT claim. The following remain outside this
checkpoint:

- IDLE/MARK/SWEEP phase-transition entry gates under live native execution;
- a deliberately stale bytecode reader racing replacement of trace slot 1;
- broader scalar IR, spills and register-pressure shapes;
- numeric/generic `FORL`;
- compiled function entry/return, side traces and stitched traces; and
- JIT-compiled FFI call/callback paths.

Those surfaces remain fail-closed under the architecture policy until their own
native execution and lifecycle proofs are added.

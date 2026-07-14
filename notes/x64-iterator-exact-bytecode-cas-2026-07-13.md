# x64 iterator bytecode publication uses exact generations

Date: 2026-07-13

## Problem

`ISNEXT`, trace publication, trace unpatching, temporary recorder unpatching,
and blacklisting can touch the same shared bytecode. A bytecode instruction is
one atomic 32-bit word, but an opcode-only load/modify/store is not a semantic
atomic operation when another publisher replaces the complete word.

The dangerous iterator interleavings were:

1. A failed `ISNEXT` observed `ITERN`.
2. A recorder published `JLOOP(A=snapshot_slots, D=traceno)`.
3. The failed `ISNEXT` changed only that word's opcode and stored
   `ITERC(A=snapshot_slots, D=traceno)`.

The resulting operands were not iterator operands. The reverse order was also
unsafe: a recorder could publish `JLOOP` after `ISNEXT` had terminally changed
the target to `ITERC`, leaving `JMP + JLOOP` and entering native code recorded
for optimized `next` from a generic iterator call.

A second race existed in the fused `ITERN` handler. It used the following
word's `D` field as an `ITERL` branch offset. After a peer changed the target to
`ITERC`, that peer could record the now-generic loop and replace the following
`ITERL(offset)` with `JITERL(traceno)`. The stale `ITERN` then interpreted a
trace number as a biased branch offset.

## Protocol

`bc_publish_cas()` is the common exact-generation primitive. It compares and
replaces the complete 32-bit word with acquire/release success ordering and an
acquire failure snapshot.

Failed `ISNEXT` now publishes in target-first order:

1. Preserve the `ISNEXT` address and publish the current stack base for C
   recovery helpers.
2. If the target is exact `ITERN`, CAS that word to the same operands with
   opcode `ITERC`.
3. If the target is `JLOOP`, recover its immutable original `ITERN` from the
   prototype sidecar, compose the correct `ITERC`, and CAS only the exact
   observed `JLOOP` generation.
4. Retry after a conflicting writer. Each failed CAS observed another
   publisher's progress.
5. Publish `ISNEXT -> JMP` only after the target is observably `ITERC`.

The intermediate `ISNEXT + ITERC` state is supported: the generic `next`
implementation accepts the optimized internal control token. The inverse
intermediate state, `JMP + ITERN`, is not safe.

All root-trace publication now uses exact original-to-JIT CAS, rather than
special-casing only `ITERN`. Thus a completed compiler cannot resurrect an
instruction after `ILOOP`, `IITERL`, `IFUNCF`, or `ITERC` won a terminal
transition. A compiled trace that loses publication is preserved, disconnected
from the prototype/entry graph, and placed in ordinary epoch-delayed retirement
without emitting a stop event or receiving GC-pressure credit. Before the
standard `TRACE abort` notification, the trace is entry-gated but remains in
its public slot so `jit.util.trace*` and `jit.dump` can inspect the aborted IR.
After the callback, cleanup re-resolves the exact slot before disconnecting it;
this makes a reentrant callback flush idempotent while every `TRACE start` still
has one terminal event.

The opposite winner order is gated too. A failed `ISNEXT` which observes a
published `JLOOP` validates its exact trace body under an SMR read lease and
atomically sets `TRACE_ENTRY_INVALIDATED` before the exact `JLOOP -> ITERC` CAS.
All x64 trace-body lookup entry paths reject that bit, as do C-side recorder and
assembler validation and new link publication. Pre-existing direct native links
need not reload it: the body remains allocated and executable until dependency
closure and an `EXIT_TRACES` boundary. The bit is deliberately separate from
`TRACE_SCOPE_FLUSH_PENDING`, so invalidation can close new validated entry
without pretending that those later retirement steps already happened. A later
function/trace/full flush promotes the root into the ordinary scoped transaction
and retires it safely.

Root setup also uses one generation throughout the TRACE-start callback window.
`trace_start()` captures and validates the complete root word before the
callback. For `ITERN`, it additionally captures the exact following `ITERL`
and validates the fixed `ISNEXT ... ITERN ITERL` tuple: matching A operands,
in-prototype targets, and an `ISNEXT` branch back to that exact `ITERN` word.
Recorder setup never reloads the root word. A token-private one-shot flag makes
the first `ITERN` record consume the captured instruction; later loop trips
load the current shared bytecode normally. If callback code terminally changes
the target to `ITERC`, recording remains internally coherent and the final root
publication CAS loses cleanly.

RECORD callbacks may themselves enter and exit pre-existing RET/ITERN traces.
Nested VM/GC-event exits and exits that do not own the exact recorder token use
the immutable start instruction for static redispatch and return `-17`; they do
not arm temporary patch fields. Only the exact token owner, outside such a
callback, may mutate `patchpc`, `patchins`, or `bcskip`. Thus callback activity
cannot skip the captured first `ITERN` or race the outer recorder's state.

Trace unpatch, pending repatch, and trace-exit temporary unpatch likewise use
exact full-word CAS. `blacklist_pc()` snapshots full words, changes `ITERN` to
`ITERC` conditionally, and recovers an adjacent `JITERL` through its immutable
sidecar before treating its `D` field as a branch offset. With the `ITERN` word
at position `p`, the following `ITERL` body is `p + 2 + j`; the `ISNEXT` guard
is therefore `p + 1 + j`. Blacklisting checks that guard's A operand and branch
target before conditionally changing it to `JMP`.

Finally, fused `ITERN` loads and decodes the complete following word once. A
captured `ITERL`/`IITERL` branches using that captured offset. A captured
`JITERL` enters its static handler, which converts the already-returned key into
the generic control value and then validates or recovers the trace generation.
That conversion is required because the real competing transition has already
made the next trip use `ITERC`.

## Tests

`tests/t-jit-startins-sidecar.c` deterministically covers:

- an `ITERN -> ITERC` CAS collision with a synthetic intervening `JLOOP`,
  including exact operand preservation and target-before-guard publication;
- stale fused `ITERN` observation of a following `JITERL`, with reserved trace
  zero forcing immutable-sidecar recovery before the original loop branch;
- root trace publication losing an exact `LOOP -> JLOOP` CAS to terminal
  `ILOOP`, including immediate removal of every runnable trace edge and paired
  start/abort event delivery, with the completed body visible to `traceinfo`
  during the abort callback;
- a TRACE-start callback re-entering the same generic-for function and forcing
  its live `ISNEXT -> JMP`, `ITERN -> ITERC` transition before recorder setup,
  including exact terminal words and absence of a runnable stale root;
- a fully published ITERN root invalidated through the real x64 failed-ISNEXT
  path, including entry-gate rejection and successful later function-scoped
  retirement;
- a RECORD VM-event callback entering and exiting an exact live native ITERN
  root while another root is being recorded, including preservation of the
  outer recorder's `patchpc`, `patchins`, `bcskip`, and captured first ITERN.

The focused `m6_jit_token` suite passes with the new fixtures.

## Performance scope

The normal successful `ISNEXT` path is unchanged. Exact CAS and sidecar lookup
are confined to failed specialization, trace publication/unpatch, or a stale
generation. Fused `ITERN` adds one complete-word decode of its following loop
instruction so opcode and operand cannot come from different generations.

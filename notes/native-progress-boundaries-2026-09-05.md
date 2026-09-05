# Native progress boundaries, 2026-09-05

Two real Linux schedules establish remaining progress dependencies at
`b4e26564`: ordinary native return waits for a consumed handshake's leader,
and first external attachment can wait for a pre-MT native loop's natural
exit. These are diagnostic results, not runtime repairs or release evidence.
The current waits protect live state; replacing them requires explicit
completion and reader authority.

Commands, probe sources, raw results, earlier fixture corrections, and source
identities are retained in
[the evidence directory](evidence/native-progress-boundaries-2026-09-05/artifact-manifest.json).
All 35 entries named by the native-ack manifest were hash-checked. Reviewed
source copies also match `git show b4e26564:<path>`; the evidence inventory
retains their hashes rather than duplicating the production files. Executable
and runtime archive hashes are recorded; binaries remain local artifacts.

## Consumed native acknowledgement

The assertion-enabled probe uses a real attached peer, physical TLS actor,
state owner claim, MARK cycle, and
`lj_safepoint_handshake(SCAN_ROOTS|FLUSH_SSB)`. Linker wrappers pause actual
admitted calls without injecting request, epoch, pending, or ownership words.
The owner returns through ordinary `lj_native_leave_l`.

| Paused leader position | Current claim? | Pending targets | Owner observation |
| --- | --- | --- | --- |
| Before admitted remote root scan | No | 2 | Indefinite poll futex |
| After root scan, before action claim/application | No | 2 | Indefinite poll futex |
| After epoch claim, during actual remote SSB application | Yes | 2 | Indefinite poll futex |
| After all per-TG actions, before global root scan | Yes | 0 | Indefinite poll futex |

In all four fresh processes, `/proc/self/task/<owner-tid>/syscall` identifies
`FUTEX_WAIT_PRIVATE` on that exact TG's poll address with expected value 1 and
a null timeout. The owner has closed native depth, retains its exact Lua state
claim, and has not returned. After releasing the real leader, native return
finishes, its source TValue is unchanged, and the marked live table survives
a full collection and value check. Another collection reaches IDLE and close
finishes. All four processes pass in about 4 ms each on CPUs 0–15, with an
outer 25-second bound. These durations describe the fixture, not a throughput
or long-pause measurement.

The strict runtime archive is
`5d404c4a30b19e7b2781caa797675b4b400e55fdba058f228ff6083664134225`.
The accepted probe source is
`e7993ffdfd42218236986f960d95765b865d7a3f05b4637dbf4c7b33be8b5946`.
No sanitizer or broad suite was rerun for this diagnostic. Earlier harness
failures are preserved and qualified in
[the full review](evidence/native-progress-boundaries-2026-09-05/native-ack/review.md):
an invalid immediate-dead-TG-reclaim assumption and a wrapper that rearmed
during the fixture's own later collection.

`hs_epoch_ack` is claimed before owner-private action application, so equality
with the current epoch is not an action-completion receipt. An empty reqmask
also represents a claim. The remote scan may still use stack geometry, frame
chains, LexState, anchors and backing storage, tmpbuf, temporary values,
NEEDSCAN handoffs, and FFI/JIT session roots. Its access cannot be revoked by
the owner merely closing native depth or rechecking a sequence afterward.

The post-action tail is a separate proof target. Full `SCAN_ROOTS` still reads
recorder IR/snapshot buffer geometry globally. `EXIT_TRACES|FLUSHJ` still uses
the consumed poll to certify generated FFI frames and retire code safely.
Allocator/phase transitions and metadata grace have their own obligations.
A per-target completion receipt alone does not justify releasing those holds
or advancing a global completed epoch. The narrow next candidate is the
actual `SCAN_OWNER_ROOTS|FLUSH_SSB` action class with unchanged phase/cycle,
after proving all later leader accesses and late SSB publication safe. Any
receipt should land with a real consumer and a paused-after-action progress
test; unused state is not a completed progress improvement.

## First external attachment during a mode-0 trace

The normal-runtime probe warms a finite numeric loop and observes a real
native exit. Before starting peers, version 2 directly verifies runnable root
trace 1 contains one `IR_XPOLL`, with literal mode 0. GC is stopped to isolate
attachment from collection. An external pthread waits for the actual main TG
`jit_base`, then calls `lj_threading_attach` on a rooted child state. Another
pthread observes the real pending transition; neither writes handshake or
phase-gate words.

During the ten-billion-iteration native loop, these values remain true for a
continuous 200.146 ms: native active, poll 1, reqmask 640, handshake leader 2,
phase gate 1 (open), `mt_entering` 1, `mt_active` 0, attachment unfinished.
Attachment completes after 3521.647 ms, at the loop's natural exit. The exact
numeric result is retained, attachment/detachment finish normally, and close
succeeds. The fresh process exits 0 in 3.525 seconds under a 30-second bound
on CPUs 0–15. Version 1 independently observed the same ordering but did not
inspect the IR literal; it remains explicitly separate evidence.

The normal runtime archive is
`9e491c5d64b1fea961c22c64d269586efa7fb2856023f373e3cb1ddb71ef7e9d`.
Version 2 probe source is
`83f5a70b3e936c32fa79830f840bc6b5ada0f30ce2efb06c4cecdabd2ba8b79f`.
The finite loop bounds the test; it does not establish bounded attachment
responsiveness. Current mode-0 assembly checks only `jit_phase_gate`, while
the generic trace-flush handshake publishes the separate TG poll and leaves
that gate open. Without natural exit or an independent GC close, the pending
attachment has no observed poll path. MT admission still follows the flush,
so this diagnostic does not demonstrate concurrent stale cdata methods.

## Automatic GC continuation prerequisites

The accompanying
[source audit](evidence/native-progress-boundaries-2026-09-05/automatic-gc/review.txt)
identifies three caller-stack continuations that cannot simply become an
asynchronous request followed by worker-token release:

- MARK activation retains worker ownership through initialization and its
  handshake. Ordinary worker admission does not exclude a live initializer
  token. Persist the activation stage and prevent early drains; helpers must
  not repeat resets, cycle increments, or carried-work consumption.
- Root snapshot leaves `root_scanned == 2` while cycle/actions and completion
  remain local to a synchronous driver. Persist exact request/cycle evidence
  and retain retries and NEEDSCAN handoffs without duplicate private actions.
- SWEEP grace relies on complete epochs while excluding a second retirement
  producer. Independent helpers need an exact retirement frontier; a later
  retirement cannot borrow an earlier completed grace, and rescue SSB work
  must drain before destruction.

A bounded outer GC step count does not bound these nested handshakes. The
dispatcher also owns target counts, late attach/detach, global roots, trace
quiescence, retirement, and final poll clear. Preserve these obligations while
moving completion into shared state. These results make no Windows/macOS or
full nonblocking-runtime claim.

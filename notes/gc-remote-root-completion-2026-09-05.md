# Remote owner-root action completion, 2026-09-05

A native owner can now resume after the unique handshake leader finishes its
exact `SCAN_OWNER_ROOTS|FLUSH_SSB` action, before that executor drops its pending
slot or finishes the leader's remaining work. The change is deliberately narrow.
Local acknowledgements, duplicate claim losers, active scanners and broader
root/trace/allocator/phase actions retain their existing holds. Automatic GC
and the handshake dispatcher are still synchronous.

Only the remote executor is eligible, after successful epoch claim and complete
action application. Eligibility also requires the same nonzero MARK cycle,
no active `jit_base` or positive VM state, and no DEAD/STOPREQ_FRESH flag. These
samples narrow eligibility; they do not pin the collector phase. Root scanning,
fresh SSB installation and allocation-accounting flush all precede completion.
The actual release is the existing `poll=0` store and futex wake, while this
executor still owns its pending slot and the leader excludes a newer epoch.
The existing counted-poll repair is retained. No new TG state, allocation,
receipt, lock or wait primitive is added; test hooks compile only with helpers.

The remaining leader work has separate authority. This exact mask excludes
global recorder-root scanning, trace retirement and allocator mode changes.
Registry walks use atomic fields; a live leader prevents physical TG teardown,
and final walks retain tactical SMR across leader release. Reclamation requires
its own metadata/entry/reader admission and detached tickets. Published SSB
nodes retain their references; the final leader-local flush selects the
physical executing TG. New owner work remains visible through ordinary active
SSB cursor checks, barriers and subsequent phase actions. Local completion is
neither a root fixed-point certificate nor a proof that SSB work is empty.

The earlier proposal also allowed a local action winner to clear its poll.
That is rejected. Production startup and GC-worker detach can call the local
poll path while native depth remains one. A local executor can consume request
A before claiming its epoch; the leader can publish and consume duplicate B
and begin a remote root scan. The local executor may then win the claim and
finish its own actions while that remote scan is still reading private state.
The current ack epoch and an empty request mask cannot prove that scan ended.

The independent deterministic regression uses the real
`lj_tg_detach -> lj_safepoint_poll_tg` caller at native depth one. It holds the
remote executor inside actual root traversal at `lj_lex_gc2_markroots`, after
the DEAD/tmpbuf checks. The rejected `b068b63f…` source reports three counted
publications, one remote scan, one local scan, local clear one and teardown
overlap one, then aborts its assertion. This demonstrates premature private
teardown, not an observed use-after-free. Adding only `native_parked &&` retains
the exact kernel poll wait, with local clear zero and no overlap. The final
source additionally documents this unique-executor requirement. Local claim
losers remain conservative even if an earlier winning action has finished.

The final permanent fixtures require:

- Six remote schedules: return with the executor paused, wake an already
  kernel-parked owner, a new counted flush epoch, STOPREQ, late attach/detach,
  and a real preserve-abort transition to IDLE while the old leader is paused.
  The resumed owner changes a rooted table and publishes a below-capacity SSB
  suffix; later full collection preserves the value.
- The local-native duplicate schedule above, retaining the required scan hold.
- Four unchanged full-root schedules, including an outstanding scan, claimed
  but unfinished action, and completed target action before global roots.

The subsequent-epoch tests check actual effects, exact ack and settled requests,
rather than assuming one signal or a nonzero final return mask: a valid same-
epoch duplicate may be the last slot consumed. The safepoint scaffold's setup
also now accepts either embedded SSB node as active after full collection. It
requires distinct active/free nodes, correct owner/storage bindings and a
terminated free list, retaining all later capacity and handshake assertions.

Final integration starts from `35cfaaa2`, with only `lj_safepoint.c` and its
helper header changed among all 224 runtime/generator inputs. Normal, assertion
and Clang ASan builds share those exact inputs. All 54 commands and 36 executed
test processes pass: stock 387 JIT-off/509 JIT-on in each build, callback stack
relocation/unwind and actual generated remote pointer/bool/sret GC/flush in all
three; the new schedules and complete safepoint scaffold in assertion/ASan.
ASan/LSan uses `detect_leaks=1:abort_on_error=1`, no suppressions, with runtime
instrumentation and uninstrumented host generators checked. The new shared
canonical `m3_safepoint_native_completion` entry passes and restores the default
build. The separate remote-flush fixture commit `35a255f0` changes only tests.

Final source SHA-256 is
`9779c6607135ffd7e06d6c1834022ad77cf491d8a30df6b63f9e5516d045af03`;
header SHA-256 is
`328aa2bc63d753257d8adca5a5417ccdda059b942138aecda20a3da53752d46c`.
[Final source and validation identities](evidence/gc-remote-root-completion-2026-09-05/combined/final-validation.json)
and [the full evidence manifest](evidence/gc-remote-root-completion-2026-09-05/artifact-manifest.json)
retain all commands, binaries, source boundaries and negative controls.

The `rejected/` material is historical proposal evidence, not approval of its
local-winner completion claim. Its own final-v4 manifest reports a failed ASan
fixture equality assertion. That manifest's fixture hash predates the later
effect-based correction; both identities and this mismatch are explicit in the
archive. The independently frozen corrected input and final integrated tests
have separate verified identities. Earlier owner-winner/duplicate progress
modes and the no-poll-repair negative do not establish coverage for the final
remote-only scope. The receipt-only draft was never built; waking an unchanged
poll word would leave a lost-wake window.

This removes one completed-target wait window. Mutable-root borrowing,
local/duplicate holds, broad global-root and trace boundaries, worker ownership,
and asynchronous phase conversion remain open. No benchmark, wait-free,
whole-runtime nonblocking or release claim is made. Linux x64 only.

# Deferred retirement after a root publication CAS loss

Date: 2026-09-05. Linux x64 validation uses isolated baseline
`006da91103b36da37811cd95917e94ccccd94f58` plus this change in `lj_trace.c` and
`lj_jit.h`. The concurrent arena-statistics and GC coalescing work is excluded.
This fixes the root-CAS-loser cleanup dependency after an ordinary ABORT event;
it does not establish that all JIT control or resumed Lua is nonblocking.

`trace_save()` commits a complete body, ownership-spine publication and exact
public slot before a root's bytecode CAS. A terminal bytecode writer can win
that CAS. Previously, `trace_stop()` invalidated the loser, delivered ordinary
`LJ_TRERR_RETRY`, then entered a looping SMR reader to retire it. A suspended
unrelated IDLE reclaimer could keep that final reader waiting while the
recorder still owned its token. Replacing only that reader with a try would
leave the invalid trace rooted indefinitely; ordinary retirement also enters
readers while preserving its semantic graph.

The new publication is one atomic byte update setting
`TRACE_ENTRY_INVALIDATED | TRACE_RETIRE_PENDING` on the exact committed body.
It occurs immediately after the failed root CAS, before optional debugger
allocation, perf-map I/O or arbitrary Lua. At that point the uninterrupted
recorder token excludes peer retirement and slot teardown. The failed CAS has
never enabled this root's bytecode entry. No other recorder can install an
inbound link before invalidation, and subsequent native/assembler admission
rejects the invalidated body.

The pending descriptor is the flag on the still-reserved public slot. It does
not add an epoch-zero node to the intrusive retire list. `retire_epoch` and
`traceno` remain suitable for ordinary `jit.util.traceinfo`/`traceir` inspection
while the ABORT recorder is active. `trace_stop()` makes no source-body access
after the callback. Reentrant flush, collection, or later slot reuse therefore
cannot invalidate a cached pointer that cleanup will dereference afterward.
The existing recorder terminal path clears ownership and publishes IDLE.

`lj_trace_markvecs()` treats pending slots as semantic roots, alongside slots
with retirement claims. A fresh `lj_gc2_smr_read_tracked_try()` precedes its
first vector/body read and covers the entire walk. Production global-root
scanning already owns an outer reader; the extra guard also protects direct
callers. Tracked same-universe TLS depth matters: `read_try()` can count an
independent-universe fallback without tracking it, and that count alone cannot
justify nested reader elision. The tracked guard refuses that case. Once
admitted, nested preservation/disconnection readers increment the same TLS
depth before examining the global gate, so a later closer cannot make them
wait while retaining the reader it needs to finish.

For a pending slot, the scan preserves its full trace/prototype/KGC/snapshot-PC
graph. Its cleanup consumer then makes one recorder-token attempt and requires
IDLE, checking state again after admission. Token contention or an active
recorder leaves the graph preserved and returns incomplete root work; the
collector's existing root-retry protocol runs another pass. A pending request
does not itself keep the recorder active. Reentrant scans during ABORT refuse
consumption, while ordinary recorder termination makes a later pass eligible.

The admitted consumer rechecks the exact slot identity and pending flag before
calling existing preserved retirement. That transaction claims a nonzero
epoch, closes native-pin admission, publishes the retire-list node, disconnects
prototype/trace links, and clears or reserves the public slot. Clearing the
pending flag follows the epoch claim. Root readers load the flag before
rechecking the epoch, closing the pending-to-retired preservation handoff.
Retirement uses the existing grace epochs, optional debugger retry, native-pin
checks, ownership-spine unlink proof and exact destructor.

No semantic edge is removed when the pending flag is published. Recording is
admitted only in IDLE or cooperative MARK; `trace_save()` retains its ordinary
active-cycle trace publication. Current-recorder root retry prevents MARK
closure and sweep until construction has unwound. Future scans explicitly
retain the pending slot. Enabling recording during SWEEP would require a new
publication proof and is outside this change.

Existing lifecycle consumers retain their ordinary contracts:

| Consumer | Pending-body disposition |
| --- | --- |
| Trace-number allocation | The non-null exact slot remains reserved; it cannot be reused while pending. |
| Runtime retire-list drain | Pending epoch-zero bodies are absent from this list. Existing nonzero-epoch readiness and grace checks are unchanged. |
| Root scan with denied token or active recorder | Preserve the semantic graph and request another root pass. |
| Explicit/reentrant scoped or full flush | Existing preserved retirement clears the request when it claims an epoch, before disconnecting the public slot. |
| Trace-capacity flush | The same ordinary flush path recovers slots; requests do not create permanent namespace exhaustion. |
| VM close | The pending trace is an ordinary public slot until close-time GC teardown clears it. It is never misclassified as unpublished assembler scratch. |
| Fresh recorder initialization | `J->cur` is cleared normally; recycled slots do not inherit the pending flag. |

`tests/t-jit-root-abort-retire.c`, registered as Linux-only
`m6_jit_root_abort_retire`, uses a genuine recorded root and the existing
bytecode-CAS collision seam. Its successful cases include:

- Ordinary RETRY callback inspection through `jit.util.traceinfo` and
  `traceir`, including another inspection after a same-owner root scan refuses
  premature consumption.
- A real IDLE reclaimer paused before ABORT preparation, and a separate
  schedule that pauses it inside the delivered ABORT callback. Arithmetic Lua
  continuation, recorder IDLE/token release and clean scratch/SMR counts are
  verified before the fixture releases that reclaimer.
- A root scan refusing the closed SMR gate without touching the source;
  tracked nested readers surviving a locally imposed later-close schedule;
  and rejection while another independent universe owns tracked TLS depth.
- Token refusal after explicitly clearing trace and prototype marks: both
  marks are restored, the pending descriptor remains intact, and no epoch is
  claimed. A subsequent eligible scan consumes the exact request.
- Callback `collectgarbage`, `jit.flush`, and another collection, followed by
  real collection to reclamation and authentic ordinary recording that reuses
  the original number. No source-pointer cleanup remains after the callback.
- Immediate VM close with an unconsumed epoch-zero pending slot.
- Thirty forced root failures with `maxtrace=8`, crossing the automatic
  capacity-flush boundary repeatedly. Real collections consume remaining
  requests, drain the retired list and recover the public slots.

The new fixture passes GCC assertion/helper, optional
`LUAJIT_USE_GDBJIT` + `LUAJIT_USE_PERFTOOLS`, and Clang AddressSanitizer builds.
ASan runtime tests use `ASAN_OPTIONS=detect_leaks=1:abort_on_error=1`, with no
runtime suppressions. The instrumented build generators run under make with
`ASAN_OPTIONS=detect_leaks=0`; that build-only option is not used for tests.
Direct fixture compiles use `-Wall -Wextra -Werror`; Clang uses `-O1 -g
-fno-omit-frame-pointer -fsanitize=address`. The canonical M6 launcher passes
end to end, including its default-build restoration.

Existing stop-admission, immutable-start-instruction, recorder-token and
trace-retirement C fixtures pass. The start-instruction and trace-retirement
fixtures also pass ASan. JIT GC-pressure, reentrant event parking and VM-event
flush Lua checks pass. The flush fixture retains its expected intentional
handler-error diagnostic. The full assertion/helper stock suites pass all
509 JIT-enabled and 387 interpreter tests. An initial direct VM-event-flush
launch omitted `tests/lib` from `LUA_PATH`; the corrected launch and its exact
environment are recorded separately.

The isolated negative control restores only the old post-ABORT looping
reader/exact-slot cleanup block, leaving the new descriptor and fixture intact.
It stops at the fixture's deliberate 15-second SIGALRM (exit `-14`, 15.0064
seconds). A separate stopped-stack capture locates the mutator in
`trace_stop -> lj_gc2_smr_read_enter`, while the other thread remains at the
real reclaimer pause. This is expected detection of the old dependency.

The initial fixture intentionally exposed another remaining dependency: it
read `payload.offset` on every resumed loop iteration and timed out after
`trace_stop()` had already returned. Its stopped stack is
`meta_chain_capture_inputs -> lj_tab_wait_l -> lj_thr_yield` under the same
paused reclaimer. The final fixture loads that field before the pause and
continues with arithmetic to isolate JIT cleanup. The table-read dependency
is retained as unresolved evidence; arbitrary resumed Lua is not proven
nonblocking by this test. Existing token-held event callbacks, general trace
flush/capacity-control waits, and the broader GC/JIT/FFI progress objective also
remain outside this fix. Deferred cleanup needs an eligible root scan, ordinary
flush or close, followed by normal grace progress for physical reclamation.
No performance improvement or release readiness is claimed.

Frozen source trees, commands, compiler versions, source/archive hashes,
stdout/stderr, negative-control patch and stopped stacks are under
`/tmp/lj-jit-root-abort-retire-20260904-1iypx69r`. Key records are
`validation-snapshot.json`, `root-abort.json`, `control-runs.json`,
`lua-control-runs-configured.json`, `asan-runs.json`, `optional-runs.json`,
`canonical.json`, `negative-runs.json`, `negative-gdb.stdout` and
`initial-timeout-gdb.stdout`. Functional commands used CPUs 0–15 while another
agent measured elsewhere; timings are diagnostic, with no system-isolation
claim. Windows and macOS validation remains deferred until release work.

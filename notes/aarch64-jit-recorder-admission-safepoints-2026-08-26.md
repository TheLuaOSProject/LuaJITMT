# ARM64 JIT recorder-admission safepoints

The hot-loop underflow path deliberately re-executes its bytecode after the C
helper returns. Routing `vm_hotloop` through the generic VM safepoint landing
would instead risk advancing or redispatching the wrong PC. Recorder admission
now services owner work in C without changing that VM control flow.

## Admission order

`lj_trace_hot()` has three request checks:

1. Before resetting the underflowed hotcount, it checks the TG's 32-bit
   `poll`, counted `reqmask`, and independent 32-bit `profile_request`, then
   enters `lj_safepoint_ack_check()` in owner context.
2. After a successful token CAS and `J->L` publication, it checks again. A late
   request first releases the still-IDLE disposable token, which also clears
   `J->L`, and only then enters the checked owner path. The hot edge is abandoned
   after a non-throwing acknowledgement so it never reuses stale admission.
3. The protected `trace_state()` callback checks before changing START to
   RECORD or touching recorder state. A fresh STOPREQ therefore unwinds through
   `lj_trace_ins()`, which discards the unpublished recorder and restores IDLE,
   the token, `J->L`, and the ordinary TG dispatch overlay before rethrowing.

The first and third checks close the residual publication race around the
post-token check. A request published just after one check is seen by the next
protected boundary or by the recorder's existing dispatch/XPOLL path.

Root admission deliberately retains LuaJIT's full hotcount reset when a token
is contended or a late request makes the newly acquired token disposable. That
reset is contention/backoff state: restoring the underflowed value would make
the same hot edge immediately contend again and is not stock recorder
semantics. Requests which throw at the first pre-reset check still leave the
triggering slot untouched, as the deterministic STOPREQ case proves.

## Side-exit admission

Side exits use the same three-word pending predicate. `lj_trace_exit()` checks
before calling `trace_hotside()`, and `trace_hotside()` checks again after its
SMR-protected parent/exit/snapshot validation but before the first shared
`SnapShot.count` CAS. This covers both a counted `reqmask` observed before its
later `poll` store and an independent profile-only publication.

A final check follows the side recorder-token CAS. If work arrived in that
window, the path releases the still-IDLE token, leaves its trace-body SMR read
lease, and abandons the side attempt before stream revalidation, owner/slot
publication, or the post-token side-claim snapshot mutation. A pre-threshold
counter increment can occur earlier and is an independent atomic CAS; the
deterministic late case uses `hotexit=1`, where no such increment precedes the
token. The path does not acknowledge inside `trace_hotside()`: the exiting TG
still publishes `jit_base` until `vm_exit_interp`. Instead, a reqmask-only
observation release-rearms `poll`, then the x64/ARM64 VM landing clears
`jit_base`, publishes INTERP, and services TGPOLL in the established safe
order. Profile-only work is already part of the paired TGPOLL load.

The reqmask-before-poll regression uses the real serialized handshake leader,
paused by a test-only hook immediately after its release-store of `reqmask` and
before its ordered `poll` store. The side hook only records the real
parent/exit/snapshot metadata. After the owner rearms and acknowledges, the
original leader is released and performs its still-required poll store. The
leader-final `safepoint_clear_consumed_polls()` pass removes that late signal
before `safepoint_leader_leave()`, preventing an orphan poll.

Unlike the root hotcount backoff above, an abandoned side attempt does not
advance the post-token snapshot counter. The parent snapshot remains exactly
at the value observed when the deterministic request was injected.

The protected external-error branch also snapshots the complete
`LJOSerrState` immediately after `lj_vm_cpcall()` returns, performs recorder
cleanup, restores that errno/LastError pair, and only then rethrows. Cleanup is
allowed to call platform or allocator code and cannot replace the error-edge
OS state.

`lj_dispatch_call()` now uses the same pending predicate for a hot-call marker.
It still fills missing fixed parameters before acknowledging, and it now sees a
profile-only signal as well as the counted poll word.

## Test coverage

`tests/t-jit-recorder-safepoint.c` uses one-shot test-only injection points at
root entry, root token acquisition, side pre-admission, side token acquisition,
and inside `trace_state()`:

- a counted entry STOPREQ proves acknowledgement happens before hotcount reset;
- a profile-only entry request and the exact hot-call helper prove the
  independent signal is consumed without manufacturing a handshake epoch;
- a late counted REDISPATCH proves the token and `J->L` are clear before
  service;
- a late root-token STOPREQ proves the same disposable-token ordering on a
  throwing request;
- a profile-only side request exercises real parent/exit metadata and proves
  `SnapShot.count` does not advance at the outer gate;
- a real publisher paused between reqmask and poll proves the outer side gate
  rearms and acknowledges without manufacturing a second request, then proves
  the resumed publisher's late poll is cleared before leader release;
- the same real paused publisher observed after the side token proves token,
  owner and SMR state are clean before the VM exit landing consumes it; with
  `hotexit=1`, the post-token side-claim snapshot count remains unchanged;
- a counted STOPREQ inside `trace_state()` proves protected unwind restores
  IDLE/token/owner/overlay state and preserves errno even when test-only cleanup
  deliberately clobbers it.

`tools/ci/jit_recorder_safepoint_contract.sh` also locks down source ordering
and rejects a future `vm_hotloop -> vm_safepoint` shortcut on x64 or ARM64.

Native macOS ARM64 runs the entry and hot-call cases in the experimental JIT
build. It still exits before token acquisition because
`LJ_ARM64_JIT_FAIL_CLOSED` remains enabled, so no ARM64 recorder, side/stitch
trace, or native-entry claim is made here. The post-token and protected
recorder and all side-admission cases are exercised by the preserved x86_64
build under Rosetta. The source contract separately keeps the ARM64 side entry
return ahead of SMR and token acquisition and verifies both VM exit landings
clear `jit_base` before checked service.

Validated on the 2026-08-26 macOS ARM64 host:

- `tools/ci/arm64_jit_fail_closed_gate.sh`, including the new native fixture;
- `tools/ci/arm64_bootstrap_gate.sh`, including 387 stock interpreter tests;
- a thin x86_64 `LJ_TRACE_TEST_HELPERS` build and the complete new fixture;
- a thin production x86_64 tracing smoke test and all 509 stock tests;
- `tools/ci/nonblocking_jit_smr_gate.sh`.

After the independent side-admission review, the focused gates were rerun with
the extended coverage: the native ARM64 fail-closed gate, the recorder source
contract, the nonblocking JIT/SMR contract, a helper-enabled x86_64 fixture
(including ten repeated Rosetta runs), and a non-helper x86_64 production
side-trace smoke all passed. The repeated fixture includes the real paused
serialized leader at both the outer side gate and the post-token gate, plus the
late-poll clear-before-leader-leave assertions. The full bootstrap and
stock-suite counts above refer to the preceding root-admission validation; they
were not rerun for this focused follow-up.

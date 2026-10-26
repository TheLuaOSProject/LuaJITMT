# ARM64 JIT callback recording notes — 2026-10-26

## Finding and change

Legacy VM-event callbacks used the global `HOOK_VMEVENT` bit to suppress
dispatch. While a callback ran on one TG, a peer recorder skipped a bytecode;
its recorder stack then disagreed with the VM stack and could assert in
`rec_check_slots`. The overlap fixture also exposed deterministic exits in the
legacy/profile callback overlap: `/tmp/ljmt-callback-overlap-before.log`
records status 134 for both legacy and profile cases.

The ARM64/x64 TG-local path now reserves `HOOK_ACTIVE|HOOK_VMEVENT` on the
callback's TG, while retaining global `HOOK_ACTIVE` as the nonblocking
exclusion for ordinary hooks and other protected callbacks. The existing
global VM-event owner still serializes legacy callbacks. Competing or
recursive callbacks drop without waiting; the non-TG-local path for other
architectures remains unchanged. The focused overlap and rooted-upvalue run
passed in `/tmp/ljmt-callback-overlap-focus-5.log`.

An earlier sporadic `rec_check_slots` assertion was captured at process 56 in
`/tmp/ljmt-upvalue-slot-run-56.log` while `reader_worker` returned through
`CGET`/`CALL` and began the next `GGET`. The diagnostic showed a stack/IR type
mismatch in the reader wrapper. The callback's global dispatch suppression
provided a source-level explanation: the peer recorder could miss the CALL.
The synchronized callback-overlap fixture now exercises this interaction.

## Pending SIGPROF teardown

Stopping `ITIMER_PROF` alone leaves a blocked SIGPROF pending on another
thread. The stop path briefly installs `SIG_IGN` after successful disarm and
before restoring the saved action; this discards already-pending deliveries.
The action state distinguishes NONE, HANDLER, and IGNORE so failed restore
leaves a safe ignored disposition and a later start can reinstall the handler
without overwriting the saved original action. The deterministic blocked-signal
fixture failed before the change with exit 134 (`/tmp/ljmt-profile-pending-before.log`)
and passes afterward (`/tmp/ljmt-callback-timer-existing.log`), including
checks that a fresh post-stop signal reaches a saved custom handler when
appropriate.

The discard behavior is specified by Apple's
[sigaction manual](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/sigaction.2.html).
Failure injection covers disarm, ignore installation, saved-action restore,
and successful restart from the retained ignored disposition. No new lock,
allocation or blocking wait is added to callback admission or timer teardown.

This does not close a separate Darwin kernel race. Current Apple XNU's
`bsd_ast()` checks the process profiling timer, updates vtimer accounting, then
decrements the timer and may enqueue SIGPROF. `setitimer(ITIMER_PROF, 0)` clears
the task vtimer bit but does not rendezvous with an AST already past that
check. Therefore an AST can enqueue a *new* SIGPROF after the IGN fence and
after the saved disposition is restored. This is an inference from the XNU
source ordering, not a portable POSIX guarantee:

- [Apple XNU `kern_sig.c`](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_sig.c), `bsd_ast()` profiling timer path.
- [Apple XNU `kern_time.c`](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/kern_time.c), `setitimer()` disarm path.
- [Apple XNU `task.c`](https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/kern/task.c), `task_vtimer_clear()` and `task_vtimer_update()`.

The concurrent `t-profile-stop-native` SIGPROF 155 termination reproduces on
both baseline and patched sources. No portable userspace drain/barrier for an
in-flight AST was identified; the pending-signal test proves only that a
signal already queued before stop is discarded.

## Other baseline issue

The x64 release `t-jit-token.c` fixture also panics with an unprotected
`not enough memory` error on clean baseline `37d0baef` (profile hash
`6b3723229cbb3670d3101aca0465c17bcf5e164a653807f5915039ee8f14ccf3`). See
`/tmp/ljmt-callback-token-baseline.log`. Treat this as an independent baseline
fixture failure, not evidence about callback recording.

## Validation status

The new CI contract intentionally selects `--callback-cleanup` from
`t-profile-stop-native`, leaving its default concurrent timer case intact. It
runs the full x64 token fixture in the assertion profile; the release panic
above remains open. Both runtime modes run the callback overlap, owner,
flush-stream, pending-signal, timer failure-injection, and cleanup fixtures.

The corrected seven-build callback contract passed uninterrupted inside
`/tmp/ljmt-callback-experimental-callback-recording-resume.log`. It covers
ARM64 and arm64e/BTI assertion/release, x64/Rosetta assertion/release, and
ARM64 with the JIT compiled out. The no-JIT invocation explicitly selects
`--interp-only` in the blocked-peer test. Earlier combined testing stopped
because that mode flag was attached to the wrong invocation in the harness;
the correction changed no production source.

Initial driver: `/tmp/ljmt-callback-full.sh`; continuation:
`/tmp/ljmt-callback-full-resume.sh`. Their matching `.log` files retain the
different tracked-diff identities and the identical production-source hash.

Both drivers used base `37d0baef885c7eafe986a73f813457ae0b7342c5` and production
diff SHA-256 `c0dc9606848841a6d2d3c7747abb3ad128bf2203e93916b972b09fceea193114`
(`git diff --binary HEAD -- src`). The initial tracked diff was
`393792ec49d3801534f1f3a4219c1593c6e439f33ae8b747712520048b6438a0`;
after correcting the two harness invocations it was
`360c385d6744f6d2353c1da43424208238d5d3dcda0c124b8006f6af71f83973`.
The continuation exited zero after checking that its HEAD, tracked diff and
production diff were unchanged. This is combined phase coverage across two
runs, not a claim that the whole experimental umbrella passed uninterrupted.

- The initial run passed the experimental gate's earlier phases through all
  four rooted-upvalue profiles, including the complete rooted-GC suite.
- The continuation passed the corrected callback matrix and all remaining
  experimental phases, including native root/side lifecycle, 222 numeric mode
  executions and all four complete M7 scalar FFI suites.
- ARM64 stock source and original-v2 import each passed all 509 cases.
- Signed x64/Rosetta passed the shared rooted table/upvalue helpers,
  lightuserdata segment-map race, and both 509-case stock corpora. Its runtime
  and affected helper objects imported no compiler atomic fallback.
- The complete bootstrap gate passed: 387 applicable stock cases, native
  roots/GC2, safepoints, tmpbuf, threading/coroutines, ARM64/arm64e callback
  lifecycle, aggregate/variadic interpreter FFI and x64 errno/result checks.

The corrected callback contract's profile identities were:

| Target | Runtime | Profile SHA-256 |
| --- | --- | --- |
| ARM64 | Assertions | `8b5eeab72d23c994cda1f84ef9444147c6b30777e6e450c3649a102339094fba` |
| arm64e/BTI | Assertions | `24051b7ae14ee34db882ecd4a29a4d3ef143f05e373831035a6bb07463ee09dc` |
| ARM64 | Release | `9b636bbdd7c3178936b57a3672d471620d07f2d8b1ec82cf696c300a0efd47c5` |
| arm64e/BTI | Release | `db1a85811ba2a3b9ff3b594fad71ef00127483fdf6366994342a697dd23e0374` |
| x64/Rosetta | Assertions | `4f7852de141983c2f467bb3546a4d1fbf7deb0f5978ba9c40350ac304a14d725` |
| x64/Rosetta | Release | `54206c70b13e04d67e11ed1a6b4ece92acafd74f88910bfa8feb31f75c4d4796` |
| ARM64, no JIT | Assertions | `099a45e06f29ba1ca9063bc57aed88340a4baed356039b0839424ce0694d8e9a` |

The Apple SDK compiler, deployment target 13.0, two build jobs and ad-hoc link
signing were used. Logs for the continuation are:

- `/tmp/ljmt-callback-experimental-callback-recording-resume.log`
- `/tmp/ljmt-callback-stock-source-callback-recording-resume.log`
- `/tmp/ljmt-callback-stock-import-callback-recording-resume.log`
- `/tmp/ljmt-callback-x64-regression-callback-recording-resume.log`
- `/tmp/ljmt-callback-bootstrap-callback-recording-resume.log`
- `/tmp/ljmt-callback-development-profile-callback-recording-resume.log`

The runtime was restored to the ordinary ARM64 experimental assertion profile,
with test helpers compiled out, profile
`fab059159e7f2058dcb56556c9029ee0494e352490761ac1d6b213b9e9291267`.
Documentation was updated after the frozen run.

A review follow-up added one test-only transition to
`tests/t-posix-signal-safety.c`: retained IGNORE, handler reinstallation,
failed timer arm, safe inert HANDLER, successful retry without another
sigaction, and eventual restoration of the original application handler.
Retaining HANDLER after a failed arm is intentional ownership recovery; the
inactive state and NULL global pointer make it inert. The extended fixture
passed on ARM64 assertion and x64 release builds in
`/tmp/ljmt-callback-ignore-retry.log` (driver
`/tmp/ljmt-callback-ignore-retry.sh`), with profiles
`101d35b2250cc6ad6f1c3b4c7b45ebf45f9017553f4935f42405ae5544b6bcf5` and
`60f37b34912d9029174d0cbba3cd4f459f2065494fa54600ee3ef19172505d29`.
Production-source hash remained unchanged, and the driver restored the ordinary
ARM64 profile on exit. This extra test was added after the frozen combined run.

## Remaining port work

This fixes recorder continuity during peer callbacks and already-pending
signal cleanup. It adds no new native bytecode capability and does not finish
the ARM64 port. General captured-local CGET/CSET cells still need rooted
recorder sampling and separate runtime/admission proofs; rejecting their
raw-cell IR at assembly does not itself prove the earlier recorder access.
The existing wider JIT/FFI admission limits and the two baseline failures
above remain open.

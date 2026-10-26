# x64 VM-event test-hook unwind repair

The release-mode `t-jit-token.c` out-of-memory injection previously escaped
both protected calls and panicked. It reproduced on clean `37d0baef` and on
the callback-recording checkpoint. The preparation test hook now has a
distinct `LJ_NOINLINE` frame. The complete release token fixture passes again,
including the expected `LUA_ERRMEM`, fixed error string, released recorder
ownership and clean retry. Its release invocation is restored to the callback
contract alongside the assertion invocation.

This is a test-helper change under `LJ_GC2_TEST_HELPERS`; ordinary builds omit
the hook. It does not change the VM error protocol, general x64 frame-pointer
policy, or ARM64 admission. The separate Darwin late-SIGPROF shutdown race and
the unexplained repeated-upvalue timeout remain open.

## Evidence and scope

The preserved diagnostic binary is `/tmp/ljmt-callback-token-diagnostic`.
Breaking on `inject_flush_setup_error` with its stage argument equal to
`LJ_VMEVENT_TEST_AFTER_EVENT_LOOKUP` identifies the exact failing throw.
`_Unwind_RaiseException` returned END_OF_STACK (5), without reaching
`lj_err_unwind_dwarf` or the protected C landing.

The live `lj_vmevent_prepare_try` caller return address to
`trace_flush_callback_cp` was at RSP+136, matching its six saved registers and
0x58-byte local allocation. Its FDE at the indirect callback return PC instead
selected RSP+152. That word pointed into an unrelated previous call, producing
an invalid unwind chain. The preparation function's inlined stage branches
shared push/pop sequences with an extra 16-byte CFA adjustment. The specific
stack/metadata mismatch is observed; it is not a claim about every compiler
version or every possible external-unwind failure.

Temporary object comparison changed only the helper's noinline attribute.
The baseline reproduced the excess CFA displacement. The changed caller used
a fixed CFA and a separate helper FDE. The complete linked release fixture
then passed with the same attribute in the source checkout. Exact LLDB
addresses, object offsets and compiler commands remain in
`/tmp/ljmt-x64-unwind-evidence.txt` and
`/tmp/ljmt-vmevent-noinline/RESULTS.txt`.

## Validation

The focused full release-token run passed in
`/tmp/ljmt-vmevent-noinline-token-release.log`, profile
`09ceb1dca6abbc291d2cb789364cf461b777f9af536fc9e209e12049c20776b7`.
The passing executable and profile manifest are preserved as
`/tmp/ljmt-vmevent-token-release-fixed` and its `.profile` file.

The seven-build follow-up uses base
`0ae771378c889a7181d7665cc51417ec345fd9cf` and tracked-diff SHA-256
`4144bc48286ec35fd7aab9333290edf70c7338531ca8b66beea76d1ed09b19ff`.
Its logs are `/tmp/ljmt-vmevent-noinline-matrix.log` and
`/tmp/ljmt-vmevent-noinline-matrix-identity.log`.

The full seven-build contract passed uninterrupted, including the complete
token fixture in both x64 modes, the added IGNORE/failed-arm retry test in
every applicable mode, both synchronized recording-overlap cases, pending
signals and callback cleanup. The four ARM64/arm64e profiles also ran the
original rooted-upvalue regression, and the no-JIT profile ran the interpreter
subset. The driver exited zero after checking unchanged HEAD and tracked diff,
then restored the normal ARM64 experimental assertion runtime.

The x64 assertion profile was
`37de3290ee4fa737ab8cef09bd9767030a19b8be9b7666d8d95ae13d2b834eac`;
the x64 release profile was
`76e91a12547060ca6e30a89733b9de58b6b928d82ee56aca7559c01db7928fc8`.
The log retains the other five profile identities. This focused follow-up did
not repeat the full experimental/bootstrap umbrellas: their earlier coverage
is recorded in
[`arm64-jit-callback-recording-2026-10-26.md`](arm64-jit-callback-recording-2026-10-26.md).

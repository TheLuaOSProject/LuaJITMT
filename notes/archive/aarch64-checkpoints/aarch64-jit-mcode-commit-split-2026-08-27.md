# AArch64 JIT split mcode commit (2026-08-27)

## Purpose

The first-side publication suffix cannot call the legacy combined
`lj_mcode_commit()`: it advances the allocator's committed top and then changes
the area's protection mode. On generic platforms that protection operation can
fail, while on macOS MAP_JIT it changes the current thread's JIT write-protect
state. Advancing `mctop` before the final seal also makes ordinary
`lj_mcode_abort()` unable to recover the previous reservation.

## Split transaction

`lj_mcode_commit_prepare(J, top, plan)` now validates that `top` lies inside the
exact current backwards-growing reservation, transitions the area to RUN while
rollback is still legal, repeats the validation, and captures the exact old
top, new top and reservation generation in `plan`. Success deliberately leaves
`J->mctop` unchanged. Calling `lj_mcode_abort()` after preparation therefore
discards the entire reservation exactly as before.

`J->mcreserve_generation` is a recorder-token-owned monotonic ticket. Odd
values mean that exactly one reservation is active; even values mean none is
active. Reserve, commit/abort and current-area replacement advance it without
wrapping. A successful publish requires the exact captured odd value and then
closes it to the next even value. Thus prepare, abort, and reserve-again cannot
revalidate a stale plan even when the allocator reuses the identical area,
old-top and new-top tuple. Replaying an already published plan is rejected for
the same reason.

`lj_mcode_commit_publish(J, plan)` performs only the bounded final `mctop`
store and ticket close. It requires `J->mctop` still equal the captured old
top and the ticket still equal the captured active generation, repeats the
exact reservation geometry check, and fail-stops on an impossible post-seal
mismatch. It contains no allocation, SMR operation, Lua error, callback or
protection change. The existing `lj_mcode_commit()` is preserved as prepare
followed by publish, so root-trace behavior is unchanged.

For the future ARM64 first-child path, preparation belongs before
`ASM -> PUBLISH`; the store belongs at the start of the sealed suffix while its
retained SMR reader and recorder token are still held.

## Validation

`tests/t-arm64-jit-mcode-commit-split.c` first creates a real admitted root so a
MAP_JIT area exists, then proves:

- null and out-of-reservation preparations are rejected without changing top;
- successful preparation changes no committed allocation state;
- abort after preparation preserves the old top and invalidates the plan;
- a separately spawned negative process proves that the stale plan fail-stops
  after reserve-again recreates the identical pointer geometry; and
- the legacy composite and split publication produce the same exact top.

`tools/ci/arm64_jit_mcode_commit_contract.sh` statically excludes allocator,
SMR, trace-error and vmevent surfaces from the publish helper, builds the
experimental archive for ordinary arm64 and arm64e+BTI, compiles the fixture
with warnings as errors, executes each twice, and restores the ordinary ARM64
build.

This checkpoint only prepares the mcode allocator boundary. It does not enter
PUBLISH, transfer trace ownership, publish a child or open side recording.

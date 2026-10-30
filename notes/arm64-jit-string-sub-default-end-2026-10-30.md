# Bounded substring with an omitted end argument

This slice admits `string.sub(s, first)` when the source function has a formal
string argument and a literal positive `first` argument. The recorder supplies
the stock default end value `-1`; the existing semantic and post-RA proofs then
derive `end = string_length`, verify the dynamic width and nonnegative-width
guard, and bind the allocation to the exact `string.sub` call and source string.
The source decoder accepts the two-argument `BC_CALL` form only with the exact
string-copy and first-index setup. Calls with an explicit end retain the prior
three-argument source grammar.

The root preflight now permits the corresponding fixed call arity at `LOOP` or
`FORL`: `CALL.C=3` or `CALL.C=4`, with the call frame and active recorder slot
count derived from that encoded arity. For the omitted-end form, a guard
snapshot can resume at the `CALL` or its preceding first-index `KSHORT`. The
proof does not read a nonexistent third argument slot. Literal `first=0`
clamping and dynamic first indices remain outside this slice.

## Validation

`tools/ci/arm64_jit_string_sub_contract.sh` passed all four profiles in
`/Users/frityet/Scratch/ljmt-sub-default-end-20261030/four-profiles-r4.log`:
ARM64 assert/release and arm64e assert/release. The existing positive and
negative-end cases passed alongside the new omitted-end cases, which cover
`first=1` and `first=2` under both `FORL` and `LOOP`. The admission fixture
accepted the unmodified trace and rejected independent semantic and post-RA
mutations to the source argument, first index, CALL result/argument arity, and
control-flow entry into argument setup. It also checked the exact callee,
substring pointer, allocation, length source, and post-RA width location.

The ordinary matched live-peer probe is
`/Users/frityet/Scratch/ljmt-sub-default-end-20261030/ordinary-sub-default-end.lua`.
The ARM64 runtime copied from the restored no-test-helper build has SHA-256
`480ea836983924e39a793772c6d5fa5dca1741cba8cdefc3907271790c76bb1a`; its
results are in `ordinary-arm64.log`. The preserved ordinary x86_64 comparator
from `ljmt-string-sub-negative-end-20261030` has SHA-256
`2028af1cb148ae10390b2364493bd2aa6424dd2a01fcdfb8989534052444b815`; its
results are in `ordinary-x64.log`. Both selected `JFORL` and `JLOOP` roots,
observed selected-root exits, compared results and error behavior, kept code
identity across GC, and completed a live-peer collection cycle.

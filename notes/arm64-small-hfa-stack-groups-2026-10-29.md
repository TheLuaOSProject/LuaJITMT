# Small HFA stack groups (2026-10-29)

Darwin ARM64 CALLXS now admits HFA groups that spill to the stack at their
natural member alignment, including one-float (4-byte) and three-float
(12-byte) groups. A group still spills wholly when the FPR bank cannot fit it,
and subsequent FP arguments remain on the stack. Integer-only aggregates keep
their separate eight-byte stack-slot rule. The interpreter aligns fixed HFA
arguments by their CType alignment; true variadic arguments retain the
existing pointer-aligned packing.

The source-plan replay and allocated post-RA replay independently reconstruct
each HFA member's contiguous offsets using its carrier width. Both preserve
eight-byte integer-composite alignment and exact group identity/member order.
Private live-trace tests mutate the selected HFA load offset and require both
admission passes to reject the altered source. Existing CALLXS ABI-plan fault
injection and other ABI/source/map/register checks continue to run in the same
contract.

The C oracle covers these stack patterns:

- Eight doubles exhaust v0-v7, followed by a scalar float at offset 0, a
  one-float HFA at 4, another scalar at 8, a three-float HFA at 12/16/20, and
  a scalar at 24. Weighted C results make every argument observable.
- Seven doubles leave only one FPR, so a three-float HFA spills at offsets
  0/4/8 as a whole and a following double starts at offset 16.
- Eight doubles followed by scalar/HFA/scalar exercise the exact one-float
  group at offset 4.

A separate AppleClang 21 caller compiled with `-target
arm64-apple-macos13.0 -O1 -S` stores these values at the offsets above. Its
assembly is saved as `apple-hfa-caller.s` under the evidence directory.

Validation:

- `arm64_jit_integer_aggregate_args_contract.sh` passed all four ARM64 and
  ARM64e assertion/release profiles on the frozen source. The new cases require
  native execution and exit evidence, compare with interpreter-only C-call
  oracles, verify effect counts through full GC, and run live semantic and
  post-RA offset mutations.
- The ordinary one-float stack peer passed on ARM64 and x86-64/Rosetta using
  `tests/t-ffi-callxs-small-hfa-stack.lua`. Each run checks a direct C result,
  candidate/interpreter equality, exact C effects, a completed loop trace with
  CALLXS, and an observed native exit after full GC. The larger mixed
  one-plus-three-HFA prototype is not admitted by the x64 JIT (`unsupported C
  function type`); this is an ARM-specific correctness expansion, not a measured
  x64 parity gap. The one-float HFA stack call does have matched x64 native
  parity.
- `git diff --check` passed. The ordinary peer skips unless run on Darwin ARM64
  or SysV x64 with `LJ_ARM64_INT_AGGREGATE_SO` set.

Evidence is in `/Users/frityet/Scratch/ljmt-small-stack-abi-20261029/`:
`apple-hfa-caller.s`, `small-hfa-four-profile.log`, `x64-build-final.log`,
and `x64-peer-final.log`. The contract restores the standard ARM64 assertion
runtime after its profile matrix.

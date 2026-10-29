# ARM64 scalar iterator roots with caller-side early break

## Scope

This change admits the canonical scalar iterator runner shape where the body
computes the next key and the caller exits with `if k >= n then break end`.
The source proof is deliberately limited to the compiler's current lowering:
`CGET k`, `CGET n`, `ISGT n,k`, a conditional jump to the next `ITERC`, and an
unconditional jump to the instruction after `ITERL`. Other conditional/jump
shapes remain closed. Existing nil-terminated ITERL admission is retained.

## Proof changes

Semantic and post-RA admission independently bind the one `IR_LT(k,n)` guard
to the exact LOOP-map visible/control value and invariant `n` slot, validate
the guard snapshot map, and require its resume PC to be the break `JMP`. The
source relation is checked in both passes; neither pass borrows the other's
acceptance result.

The second iterator call may carry a child-frame snapshot that omits the
unused `n` formal. In that case, admission reconstructs only that argument
from the exact `n` value already proved in the caller LOOP map and requires
the preserved root state slot to match it. The child control/key argument is
still required in the child map. This closes the actual late-child snapshot
refusal without relying on a nearby function guard or weakening entry maps.

## Validation

- `tools/ci/arm64_jit_iterl_contract.sh`, all four target profiles:
  ARM64 assertion, arm64e assertion, ARM64 release, and arm64e release.
  Each reports 98 selected-root calls and 72/72 independent semantic and
  72/72 post-RA private rejections. The native fixture checks bad-input,
  zero/short/full inputs, the break-JMP exit footer, full GC, flush/re-entry,
  and a live MT peer. Log: `/Users/frityet/Scratch/ljmt-iterl-break-root-20261029/four-profile.log`.
- Ordinary no-helper ARM64 build and a no-observer probe published runner
  `JITERL` root 1 and matched an interpreter oracle for 64, 0, 1, and 96 over
  repeated calls and full GC. Bundle and exact build flags:
  `/Users/frityet/Scratch/ljmt-iterl-break-root-20261029/ordinary-arm64/`.
- The same probe against the frozen x64 runtime published runner root 2 and
  matched the same values. Binary SHA-256:
  `5998c9d47d84d717619d0ef50e339550c9cbe361fd2cbf3fcf8a6fadc4cfe04c`;
  output: `/Users/frityet/Scratch/ljmt-iterl-break-root-20261029/frozen-x64-probe.log`.
- The focused C fixture exercises native selected-root entry and exit; the
  ordinary Lua probes establish matching publication, not native-entry counts.

## Limits

This does not widen general iterator tuple shapes, arbitrary loop-body
control flow, or iterator return arity. It covers the scalar `k+1` iterator
and canonical caller-side early break represented above.

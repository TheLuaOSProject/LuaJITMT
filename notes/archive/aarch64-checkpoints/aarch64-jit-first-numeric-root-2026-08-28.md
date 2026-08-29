# AArch64 JIT first spill-free numeric root (2026-08-28)

## Scope

The macOS AArch64 JIT now admits and executes one exact mixed scalar
`BC_LOOP` root:

```lua
local function f(n, x, step)
  local i = 0
  while i < n do
    i = i + 1
    x = x + step
  end
  return x
end
```

Integer `i` and `n` retain the previously certified overflow and comparison
path. Numeric `x` and `step` add the first native FPR data path: checked NUM
stack loads, loop-carried NUM additions, a NUM PHI, snapshot restoration, and
native exits. The prototype has 17 bytecodes, six stack slots, and three
parameters.

This is an exact execution canary, not general numeric admission. Numeric
constants, numeric comparisons, conversions, subtraction, multiplication,
division, spills, calls, allocation, arbitrary roots, and numeric sides remain
closed. `ffi.cdef`, `require`, and the other deliberately serialized surfaces
are unchanged.

## Exact semantic certificate

The root has one `KINT 1` plus the canonical `true`, `false`, and `nil`
constants. Its semantic IR is fixed to:

1. integer `SLOAD i` from slot 5;
2. NUM `SLOAD x` and `step` from slots 3 and 4;
3. integer `ADDOV i, 1` and NUM `ADD step, x` before the loop;
4. integer `SLOAD n` and `GT n, i` pre-guard;
5. `LOOP`, immediately followed by `XPOLL 1`;
6. integer `ADDOV i, 1` and NUM `ADD x, step` in the body;
7. integer `LT i, n` body guard; and
8. integer and NUM PHIs for the two loop-carried values.

There are exactly seven snapshots and 27 snapshot-map entries. Their IR refs,
map offsets, entry counts, slot counts, and common top slot are independently
pinned by both the synthetic and native fixtures. Every numeric snapshot value
is restored from an effective FPR location after applying any eligible
`RENAME`.

The shared scalar validator now distinguishes integer and numeric producer
families. It continues to count only integer `IR_ADD` nodes for the exact FORL
grammar, validates all constants instead of merely their range, and invokes an
additional exact-shape proof whenever NUM is present. A numeric-only root is
not admitted: this tranche requires the exact mixed INT+NUM shape above.

## Post-register-allocation certificate

The observed allocation is spill-free and has no stack adjustment:

| Value | Register |
| --- | --- |
| initial `i` | x2 |
| initial `x` | d1 |
| invariant `step` | d0 |
| pre/body integer induction and integer PHI | x28 |
| pre/body numeric accumulator and numeric PHI | d15 |
| integer limit | x0 |
| integer snapshot rename after snapshot 4 | x27 |

The gate accepts an alternative numeric FPR only when the NUM PHI and both of
its producers move together. The same equality is required for the integer
PHI triple. This matches the allocator: `asm_phi()` selects the body/right
register and `asm_phi_shuffle()` moves the head/left value into it. With spills
closed, a mismatch is not a realizable layout.

All semantic NUM values must have an allocatable FPR and `SPS_NONE`. NUM
snapshot values and NUM renames are checked by type and must also resolve to an
allocatable FPR with no spill. The current root therefore requires
`spadjust == 0` and no used spill slot.

The existing ARM64 backend already lowered these admitted operations through
FPR loads, `FADD`, FPR PHI shuffling, complete FPR exit-state saves, and NUM
snapshot restoration. No backend instruction change was needed. The admission
and restoration certificates were the missing boundary.

NUM spills deliberately remain closed. A later spill tranche must account for
their two-slot/even-slot layout and use a width-aware end of `slot + 2`; the
current generic scalar high-water calculation is intentionally only sufficient
for the admitted integer spills.

## Runtime and fail-closed proof

`tests/t-arm64-jit-numeric-loop.c` records the real root and verifies its
immutable IR, constants, seven snapshots, FPR/GPR allocation, lone integer
rename, native publication, native re-entry, type exit, and flush retirement.
The contract runs direct placement plus two randomized placements on ordinary
ARM64 and again on arm64e with BTI.

A deterministic post-admission publisher proves the native XPOLL lifecycle:

- a profile request exits at snapshot 4 while the accumulator is live in an
  FPR, restores it, returns `11.25`, re-enters the same trace, and reaches the
  final snapshot 6 exit;
- a STOPREQ published at the same point exits at snapshot 4 with the expected
  VM-shutdown error, acknowledges the new epoch, drains pending/request/poll
  state, preserves the sticky stop bit until explicitly cleared, and leaves
  the trace reusable; and
- a subsequent ordinary call again returns `11.25` natively.

Passing integer `x` takes the initial NUM type exit and then resumes once the
interpreted addition produces a number. With `hotexit=1`, this creates a real
side-recording opportunity, but the root remains childless and `nextside`
remains zero.

The runtime negatives prove that these natural programs publish no trace:

- a numeric half-step loop, which needs `KNUM` and an FP comparison;
- `x = x + i`, which needs an integer-to-number conversion; and
- `x = x * step`, which needs NUM multiplication.

The synthetic matrix additionally rejects malformed NUM loads/additions/PHIs,
forward references, KNUM, FP comparison, conversion, NUM subtraction,
multiplication and division, calls, table allocation, FORL reuse, GPR-backed
NUM values, invalid registers, NUM spills, snapshot spills, bad renames, and
each isolated PHI register mismatch.

The existing `TRACE_ARM64_INT_LOOP_ADMITTED` root marker is retained for ABI
stability. First-side preflight is independently pinned to the older integer
parent geometry (top slot 5, 19 bytecodes, and prototype framesize 5), so the
new top-slot-6, 17-bytecode numeric root cannot enter first-side publication.

## Integration fixes exposed by the umbrella

The first umbrella run found a stale FORL source pin after `nadd` became the
more precise `nintadd`; the production invariant was unchanged and the
contract was updated. The second run found that the synthetic root-entry
fixture had left the canonical KPRI slots zeroed. Initializing `true`, `false`,
and `nil` made that fixture satisfy the newly complete constant validator.

Both failures occurred in fail-closed test construction. Neither required
weakening production admission.

## Commits and validation

The implementation and proof were published incrementally:

- `79b25bcd` — exact spill-free numeric-root admission;
- `f79ca5ee` — realizable INT/NUM PHI register equality;
- `8396d931` — synthetic and native lifecycle proof;
- `b6512079` — FORL integer-add source-contract integration; and
- `5aa27e8c` — canonical constants in the root-entry fixture.

An independent review found no blocker in the semantic gate, post-RA gate,
first-side isolation, or XPOLL/STOPREQ lifecycle.

Validation completed on this Apple Silicon host:

- the focused synthetic admission matrix;
- the exact numeric-loop contract on ARM64 and arm64e/BTI, direct and
  randomized;
- the FORL and root-entry integration contracts on ARM64 and arm64e/BTI;
- the complete `tools/ci/arm64_jit_fail_closed_gate.sh` umbrella from a clean
  restart, including root/side publication, GDBJIT, callbacks, native
  LOOP/FORL/JFUNCF, exits, retirement, flush/reuse, and safepoints;
- the native ARM64 JIT-enabled vendored suite: 509 passed;
- the thin macOS x86_64 platform smoke, a real published Rosetta JIT loop, and
  the x86_64 vendored suite: 509 passed; and
- restoration of the thin ARM64 experimental build with trace helpers and
  `LUAJIT_MCODE_TEST`, followed by a native mixed-numeric JIT smoke.

The x86 builder's discarded preliminary non-GC64 diagnostic is expected. The
only compiler diagnostics were the existing x86 `lj_api.c` unused `topofs`
warning and ARM64 `lj_ccall.c` unused `ccall_rawchild_wait` warning.

## Next bounded tranche

The next useful root is the currently rejected half-step loop:

```lua
local x = 0.5
while x < limit do
  x = x + 0.5
end
```

It should be admitted as a separate exact, spill-free certificate adding only
canonical KNUM handling and ordered FP comparisons. Its proof must cover ARM64
`FCMP` branch polarity, NaN/unordered behavior, snapshot restoration, XPOLL,
STOPREQ, arm64e/BTI, randomized placement, and continued side closure.

Conversions should follow in a separate tranche. Additional arithmetic,
width-aware NUM spills, calls, heap operations, general roots, sides, and JIT
FFI remain later boundaries.

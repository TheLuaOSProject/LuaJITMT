# ARM64 JIT inclusive all-parameter pure-NUM root

Date: 2026-08-28

## Scope

This tranche admits one additional exact macOS ARM64 root while keeping the
broad recorder, side-trace and stitch gates closed:

```lua
local function f(x, limit, step)
  while x <= limit do
    x = x + step
  end
  return x
end
```

The accumulator, limit and step are all live `NUM` parameters. This is an
inclusive comparison profile over the same no-constant dynamic-NUM kernel and
prototype/snapshot geometry as the previously admitted strict all-parameter
root. It adds no backend opcode, spill rule, call path, heap operation or
publication mechanism. The fixed-initializer dynamic-step root continues to
select the strict profile explicitly.

## Exact recorder certificate

The admitted prototype is fixed to:

- `framesize = 5`, `sizebc = 13`, `numparams = 3`;
- no upvalues, numeric constants or GC constants;
- exactly `PROTO2_CELLOPS` in `flags2`; and
- root `startpc = proto_bc(pt) + 5`.

All thirteen bytecodes are checked. The inclusive profile is the exact
`FUNCF/CGET/CGET/ISGT/JMP/LOOP/CGET/CGET/ADDVV/CSET/JMP/CGET/RET1`
sequence. In particular, bytecode 3 is `BC_ISGT A=3 D=4`, bytecode 4 is
`BC_JMP A=3 J=6`, the root is `BC_LOOP A=3 J=5`, `ADDVV` is exactly
`A=3 B=3 C=4`, and the backedge is `BC_JMP A=3 J=-10`. The comparison-profile
selector accepts only `BC_ISGE` for the existing strict root or `BC_ISGT` for
this inclusive root, both with `A=3 D=4`.

The semantic IR has no constants (`nk == REF_TRUE`) and exactly ten
instructions after `REF_BASE`:

1. guarded NUM SLOAD `X` from slot 2;
2. guarded NUM SLOAD `STEP` from slot 4;
3. PHI-marked `X_PRE = STEP + X`;
4. guarded NUM SLOAD `LIMIT` from slot 3;
5. guarded `LIMIT >= X_PRE`;
6. `LOOP`;
7. `XPOLL 1`;
8. PHI-marked `X_BODY = X_PRE + STEP`;
9. guarded `X_BODY <= LIMIT`; and
10. `PHI(X_PRE, X_BODY)`.

The strict and inclusive profiles therefore remain paired exactly as
`BC_ISGE + IR_GT/IR_LT` and `BC_ISGT + IR_GE/IR_LE`. Both semantic admission
and the independent post-RA admission pass recompute this profile from the
prototype; mixed bytecode/IR profiles are not accepted.

The five snapshots are pinned to refs `X`, `LIMIT`, the preheader guard,
`LOOP`, and the body guard. Their map offsets are `{0,2,6,9,12}`, entry counts
are `{0,2,1,1,1}`, slot counts are `{5,6,5,5,5}`, `topslot` is 5, and footer
PCs are `{bc+6,bc+2,bc+11,bc+6,bc+11}`. There are exactly 15 snapshot-map
entries. The five restored entries are slots `{2,5,2,2,2}`, all with zero
flags, referencing `X_PRE` four times and `X_BODY` once. The base delta is
zero.

The post-RA certificate requires one terminal zeroed `NOP`, zero stack
adjustment, no spills or renames, and FPR-only numeric values. The loop-carried
`X_PRE/X_BODY/PHI` family must share one register. `STEP`, `LIMIT` and that
family must be pairwise distinct, and the original `X` cannot alias `STEP`.
`X` may alias `LIMIT` or the PHI family after its first-add use dies. The
canonical observed allocation remains `X=d2`, `STEP=d1`,
`X_PRE/X_BODY/PHI=d15`, and `LIMIT=d0`, while the contract also accepts legal
register renumberings and the two dead-`X` aliases.

## ARM64 lowering certificate

Ordinary ARM64 emits exactly 136 bytes with `mcloop = 76`. ARM64e/BTI emits
exactly 140 bytes with `mcloop = 80` and a leading `BTI J`. Both forms contain
exactly two double-precision FADDs and two double-precision FCMPs, with no
spilled numeric value.

For the inclusive preheader, the certified lowering compares `X_PRE` with
`LIMIT` and branches `HI` to exit 2. This rejects both an ordered value above
the limit and an unordered NaN result. The body comparison branches `LS` back
to the loop and otherwise falls through the unconditional branch to exit 4.
Equality therefore stays on the native loop while unordered values leave it.
The existing strict profile retains its `HS` preheader exit and `LO` loop
branch. No broader relational-family lowering is admitted by this change.

## Runtime and lifecycle proof

The native fixture exercises the strict and inclusive profiles separately on
ARM64 and ARM64e/BTI, in direct and randomized-mcode runs. It proves one root
is created and reused while all three live NUM arguments change. The sensitive
tuple `(0.25, 1.0, 0.375)` returns `1.0` for strict comparison and `1.375` for
inclusive comparison, so stale specialization of `x`, `limit`, `step`, or the
comparison profile changes the result.

Inclusive equality is covered at every boundary:

- `(0.25, 1.0, 0.375)` reaches equality at the loop-body comparison and must
  continue to `1.375`;
- `(0.625, 1.0, 0.375)` reaches equality at the native preheader and must
  continue to `1.375`; and
- `(1.0, 1.0, 0.375)` uses equality in the interpreter to enter the patched
  root, then exits at native preheader exit 2 with `1.375` after the first add.

All five exits are exercised without creating a side trace:

- exit 0: accumulator or step NUM type guard;
- exit 1: limit NUM type guard;
- exit 2: inclusive preheader comparison;
- exit 3: XPOLL for profile or STOPREQ work; and
- exit 4: final body comparison.

Repeated hot exits leave `trace_nchild == 0` and `trace_nextside == 0`. An
integer accumulator demonstrates exit-0 interpreter conversion and native
re-entry; integer step and limit inputs exercise their exact type exits. Trace
flush restores the original `BC_LOOP` and removes the runnable root.

Profile work and STOPREQ are published at the test-only post-admission boundary,
after the final entry-side request check and before the native target runs.
XPOLL performs the request, error, epoch-acknowledgement and leader cleanup.
Every case verifies that `jit_base` is cleared, native depth returns to zero,
the VM state is restored, pending/request/poll/profile state is empty, STOPREQ
freshness is consumed and the same root remains reusable after cleanup.

The inclusive fixture also mutates each live argument after admission and
before its native SLOAD/FADD:

- `x = qNaN` returns NaN through exit 2;
- `x = +Inf` returns positive infinity through exit 2;
- `x = -Inf` would remain negative infinity under positive addition, so a
  simultaneous STOPREQ proves termination through XPOLL exit 3;
- `limit = qNaN` and `limit = -Inf` both return the first updated accumulator,
  `0.75`, through exit 2;
- `limit = +Inf` can evolve to positive infinity and remain inclusive forever,
  so a simultaneous STOPREQ proves termination through exit 3;
- `step = qNaN` returns NaN through exit 2;
- `step = +Inf` returns positive infinity through exit 2; and
- `step = -Inf` would keep the accumulator at negative infinity, so a
  simultaneous STOPREQ proves termination through exit 3.

Each mutation is followed by full handshake cleanup, exact root revalidation
and a finite final-exit reuse. The original strict accumulator qNaN and +Inf
mutation cases remain in the same fixture as regression coverage.

## Fail-closed proof

The synthetic contract executes the full bytecode-profile x preheader-IR x
body-IR cross-product at both semantic and post-RA gates. Only
`ISGE + GT/LT` and `ISGT + GE/LE` pass. Mixed strict/inclusive pairs, whole-pair
swaps, `ULT/UGE/ULE/UGT`, other signed comparisons, and reversed comparison
operands fail.

It also independently mutates every field of all ten semantic IR tuples, all
prototype identity fields, opcode/A/D-C/B bits of all thirteen bytecodes,
start PC/instruction, trace length and constant boundary, every snapshot
header/restored entry/footer PC, `nsnap`, `nsnapmap`, `topslot` and base delta.
Post-RA tests repeat the semantic and snapshot mutations and reject spills,
GPR or out-of-range assignments, impossible aliases, split PHI-family
registers, nonzero stack adjustment, renames, a malformed terminal NOP, and
extra or missing instructions.

The native negative cases keep the established fixed-half and strict
fixed-initializer dynamic-step profiles separate. Inclusive fixed-initializer
and fixed-half variants remain interpreted. `>`, `>=`, reversed
`limit >= x`, reversed `step + x`, subtraction, multiplication, division, and
strict or inclusive extra-ADD recurrences remain outside the admitted grammar.

## Commits and validation

The tranche was published incrementally:

- `7869004a` - admit the exact inclusive semantic and post-RA profile;
- `cb9fb95c` - add the synthetic comparison cross-product and mutation
  certificate; and
- `ed4b6985` - add the ARM64/arm64e native runtime, lifecycle and exceptional
  value proof.

Focused validation completed on this Apple Silicon host:

- native ARM64 and arm64e/BTI source builds plus a live inclusive-root smoke;
- the semantic/post-RA admission contract, including native ARM64 execution and
  arm64e/BTI `-Werror` compilation; and
- two complete focused runtime-contract passes, each covering direct and two
  randomized-mcode executions on both ARM64 and ARM64e/BTI, followed by
  restoration of the ordinary ARM64 helper build.

Full validation also completed:

- the complete `tools/ci/arm64_jit_fail_closed_gate.sh` umbrella, including all
  prior root, first-side, publication, exit, retirement, GDBJIT, callback,
  flush/reuse and safepoint contracts;
- the native ARM64 JIT-enabled vendored suite: 509 passed;
- the thin macOS x86_64 platform build and smoke, including a real Rosetta JIT
  loop reporting `jit.os == "OSX"`, `jit.arch == "x64"`, and
  `linktype == "loop"`, followed by the x86_64 vendored suite: 509 passed; and
- restoration of the exact ordinary ARM64 experimental helper build followed
  by live publication of the fixed-half, dynamic-step, strict all-parameter,
  inclusive all-parameter and mixed-NUM roots, all with `linktype == "loop"`.

The recurring diagnostics were the pre-existing ARM64 unused
`ccall_rawchild_wait` warning and x86_64 unused `topofs` warning. The x86_64
platform builder's discarded preliminary non-GC64 clean diagnostic is also
pre-existing; the actual target build, smoke and suite passed.

## Boundary after this tranche

This admission proves only the exact all-parameter ADD recurrence and the two
coherent strict/inclusive comparison profiles above. It does not authorize
fixed-initializer inclusive roots, reversed operands, another relational
family, arbitrary arithmetic, spills, calls, heap effects, side traces or
stitches. Any further widening needs its own bytecode, semantic IR, snapshot,
post-RA, machine-code and exceptional-value certificate.

# ARM64 JIT inclusive ascending MUL all-parameter pure-NUM root

Date: 2026-08-28

Checkpoint: `456f3b1b`

## Scope

This tranche admits one additional exact macOS ARM64 root while keeping the
broad recorder, side-trace and stitch gates closed:

```lua
local function f(x, limit, factor)
  while x <= limit do
    x = x * factor
  end
  return x
end
```

The accumulator, limit and factor are all live `NUM` parameters. This is the
inclusive ascending `MUL_LE` counterpart to the previously certified strict
`MUL_LT` root and is the eighth all-parameter dynamic-accumulator profile.
The terminating certificate tuples use a positive factor greater than one,
but production admission adds no sign, magnitude, finiteness or progress
guard. Zero, one, negative factors and exceptional values may preserve the
source program's intrinsic nontermination.

The ARM64 backend already lowers NUM `IR_MUL` to double-precision FMUL. This
tranche does not add a backend opcode. It adds one exact source profile to the
separately gated MUL authorization and extends the semantic, post-RA,
machine-code, runtime, exceptional-value and lockless-lifecycle certificates.
It adds no spill rule, conversion, call path, heap operation, side/stitch
admission or publication mechanism.

## Exact prototype and bytecode certificate

The admitted prototype is fixed to:

- `framesize = 5`, `sizebc = 13`, `numparams = 3`;
- no upvalues, numeric constants or GC constants;
- exactly `PROTO2_CELLOPS` in `flags2`; and
- root `startpc = proto_bc(pt) + 5`.

The native source-prototype check additionally observes
`flags == PROTO_HAS_RETURN`.

All thirteen bytecodes and their operands or jump fields are checked:

```text
0  FUNCF A=5
1  CGET  A=3 D=0
2  CGET  A=4 D=1
3  ISGT  A=3 D=4
4  JMP   A=3 +6
5  LOOP  A=3 +5
6  CGET  A=3 D=0
7  CGET  A=4 D=2
8  MULVV A=3 B=3 C=4
9  CSET  A=0 D=3
10 JMP   A=3 -10
11 CGET  A=3 D=0
12 RET1  A=3 D=2
```

The comparison direction at bytecode 3 and MUL operand order at bytecode 8
are part of the profile even though multiplication is commutative. The live
root changes bytecode 5 from `BC_LOOP` to `BC_JLOOP D=1`; trace flush must
restore the trace-owned original `BC_LOOP A=3 J=5`.

The one-bytecode difference from `MUL_LT` is deliberate: strict `x < limit`
uses `ISGE A=3 D=4`, while inclusive `x <= limit` uses
`ISGT A=3 D=4`. Reversed comparisons, reversed MUL operands and every other
comparison/arithmetic combination remain different source profiles.

## Exact semantic IR and snapshots

The semantic IR has no constants (`nk == REF_TRUE`) and exactly ten
instructions after `REF_BASE`:

1. guarded NUM SLOAD `X` from slot 2;
2. guarded NUM SLOAD `FACTOR` from slot 4;
3. PHI-marked `X_PRE = FACTOR * X`;
4. guarded NUM SLOAD `LIMIT` from slot 3;
5. guarded `LIMIT >= X_PRE`;
6. `LOOP`;
7. `XPOLL 1`;
8. PHI-marked `X_BODY = X_PRE * FACTOR`;
9. guarded `X_BODY <= LIMIT`; and
10. `PHI(X_PRE, X_BODY)`.

The exact tuple is therefore
`BC_ISGT A=3 D=4 + IR_MUL(FACTOR,X) + IR_GE(LIMIT,X_PRE) +
IR_MUL(X_PRE,FACTOR) + IR_LE(X_BODY,LIMIT)`. It is selected as profile ID 8,
`ARM64_NUMDYN_MUL_LE`; it is not inferred from a generic multiplication or
relational family.

The five snapshots are pinned to refs `X`, `LIMIT`, the preheader guard,
`LOOP`, and the body guard. Their map offsets are `{0,2,6,9,12}`, entry counts
are `{0,2,1,1,1}`, slot counts are `{5,6,5,5,5}`, `topslot` is 5, and footer
PCs are `{bc+6,bc+2,bc+11,bc+6,bc+11}`. There are exactly 15 snapshot-map
entries. The five restored entries are slots `{2,5,2,2,2}`, all with zero
flags, referencing `X_PRE` four times and `X_BODY` once. The base delta is
zero.

Post-RA adds only the expected zeroed terminal `NOP`. Extra or missing IR,
constants, sinks, snapshot entries, restored values, nonzero snapshot flags
or altered footer PCs remain rejected.

## Fail-closed production design

The all-parameter classifier now has eight named full-shape profiles:

- `ADD_LT`: exact `ADDVV`, pre/body `GT/LT`;
- `ADD_LE`: exact `ADDVV`, pre/body `GE/LE`;
- `ADD_GT`: exact `ADDVV`, pre/body `LT/GT`;
- `ADD_GE`: exact `ADDVV`, pre/body `LE/GE`;
- `SUB_GT`: exact `SUBVV`, pre/body `LT/GT`;
- `SUB_GE`: exact `SUBVV`, pre/body `LE/GE`;
- `MUL_LT`: exact `MULVV`, pre/body `GT/LT`; and
- `MUL_LE`: exact `MULVV`, pre/body `GE/LE`.

`arm64_numacc_grammar_profile()` acquires the live comparison and recurrence
bytecodes and returns only one exact row. For `MUL_LE`, it requires
`BC_ISGT A=3 D=4` and `BC_MULVV A=3 B=3 C=4`. Any other operand, opcode,
strictness or direction returns profile 0.

MUL remains separate from ADD and SUB authorization.
`arm64_numdynamic_is_mul()` returns true only for
`ARM64_NUMDYN_MUL_LT` or `ARM64_NUMDYN_MUL_LE`. Semantic and independent
post-RA admission derive `allow_num_mul` only from that exact source result.
Their `IR_MUL` cases then require a `BC_LOOP` root, NUM type, exact PHI flags
and producer operands accepted by the selected profile kernel. The generic
NUM producer helper accepts MUL only when that separate flag is set.

Both independent kernels spell out the `MUL_LE` recurrence, first operand
order and `GE/LE` guards. Integer `IR_MUL`, `IR_MULOV`, MUL in any other NUM
root, conversions, DIV and all other arithmetic remain outside this
authorization. The existing fixed-initializer dynamic-step root still invokes
the shared kernel with the exact `ADD_LT` profile, so adding profile 8 does not
widen that root.

No generic arithmetic allowlist was widened. Every admitted MUL producer has
NUM type, exact operands, exact PHI flags and an exact position in the
ten-reference shape. The post-RA scan independently repeats the source,
semantic, snapshot and allocation restrictions before native publication.

## Exhaustive 512-case compiler proof

The synthetic compiler fixture has eight explicit profile records, including:

```text
MUL_LE = BC_ISGT A=3 D=4, BC_MULVV,
         IR_MUL(FACTOR,X), IR_GE, IR_MUL, IR_LE
```

For each profile it crosses two pre-arithmetic choices, two body-arithmetic
choices, four preheader guards and four body guards. The complete product is:

```text
8 profiles x 2 pre-arithmetic x 2 body-arithmetic
           x 4 pre-guards x 4 body-guards = 512 combinations
```

The same 512 combinations are presented independently to semantic admission
and post-RA admission. Exactly the eight coherent named rows are expected at
each gate. Coherence comes from the active profile fields, not profile-index
arithmetic. Each profile is crossed with its exact recurrence and one
distinct adjacent recurrence; the separate exhaustive mutation suites close
the remaining opcode families.

For all eight profiles, the fixture independently mutates prototype identity,
frame and parameter counts, flags, start PC/instruction, every bytecode field,
semantic opcode/type/flags/operand, instruction count, snapshot header/map/
footer, post-RA register class, spill, rename, PHI register relationship,
stack adjustment, aliases and terminal NOP. It also changes only strictness,
only comparison operand direction, only recurrence bytecode and only MUL
operand order. Each change must reject at both gates.

The source contract pins profile ID 8, the exact selector, both independent
`MUL_LE` kernel bodies, the exact two-profile MUL authorization, the
eight-profile invocation list, the 512-case count and exactly eight
admissions. It also rejects MUL authorization leaking to any ADD or SUB row.

This compiler proof is present in commit `4948dd31`. At the exact runtime
checkpoint `456f3b1b`, the complete fail-closed umbrella passed and thereby
executed the focused synthetic compiler contract against the integrated
tranche.

## Post-RA and machine-code certificate

The post-RA certificate requires zero stack adjustment, no spills or renames,
and FPR-only NUM values. The loop-carried `X_PRE/X_BODY/PHI` family shares one
register. `FACTOR`, `LIMIT` and that family are pairwise distinct, and the
original `X` cannot alias `FACTOR`. Both MUL operand orders are rechecked after
allocation.

The observed allocation is:

```text
X=d2
FACTOR=d1
LIMIT=d0
X_PRE=X_BODY=PHI=d15
```

Ordinary ARM64 emits exactly 136 bytes with `mcloop = 76`. ARM64e/BTI emits
exactly 140 bytes with `mcloop = 80` and a leading `BTI J`. Both forms contain
exactly two double-precision FMULs, no FADD or FSUB, and exactly two
double-precision FCMPs.

MUL no longer shares the ADD certificate's commutative machine-code allowance.
The first recurrence must be `FMUL d15,d1,d2` (`FACTOR,X`), while the loop
body must be `FMUL d15,d15,d1` (`X_PRE,FACTOR`). For the inclusive profile,
the ordinary words and offsets are pinned as follows:

```text
word 12  0x1e62082f  FMUL d15,d1,d2
word 18  0x54000488  B.HI exit 2
word 30  0x1e6109ef  FMUL d15,d15,d1
word 31  0x1e6021e0  FCMP d15,d0
word 32  0x54fffe69  B.LS loop
word 33  0x14000025  B exit 4
```

The leading BTI shifts those same words to arm64e offsets
`13,19,31,32,33,34`; their encodings remain identical.

Both comparisons use `FCMP d15,d0`. The preheader's `B.HI` exits when the
first product is greater than the limit and also exits an unordered NaN
comparison. Equality continues into the loop. The body uses `B.LS` for the
inclusive backedge, so equality loops once more. Unordered values do not take
that backedge and instead reach the unconditional exit-4 branch.

The decoder checks branch targets against the trace's exit stubs and mcloop,
not only condition-code nibbles. Direct and randomized placement therefore
exercise the same exact internal control flow without depending on a fixed
mcode mapping address.

## Runtime values, equality and type exits

The native fixture records:

```text
(x, limit, factor) = (0.5, 20.25, 2.0) -> 32.0
```

It then reuses the same root with the exact-binary live-argument tuple:

```text
(0.625, 5.625, 3.0) -> 16.875
```

Replacing only `x`, `limit`, or `factor` with its recording value produces
`13.5`, `50.625`, or `10.0`. All four reuse/substitution calls leave through
final exit 4. Their different iteration counts and results detect accidental
specialization of every live parameter. Root count, prototype ownership,
link type, IR, snapshots, allocation and machine code are revalidated after
every reuse.

Inclusive equality is covered at all three boundaries:

- `(0.5, 2.0, 2.0) -> 4.0`: body equality takes one extra inclusive backedge,
  then final exit 4;
- `(0.5, 1.0, 2.0) -> 2.0`: equality after the first MUL passes the inclusive
  precondition, then final exit 4; and
- `(1.0, 1.0, 2.0) -> 2.0`: initial interpreter equality enters `JLOOP`, the
  first product exceeds the limit, and native precondition exit 2 returns it.

The strict `MUL_LT` equality expectations remain in a separate branch and are
re-exercised as their own profile. No shared MUL branch silently applies
strict equality behavior to `MUL_LE`.

All five exits are exercised while side recording remains closed:

- exit 0: accumulator or factor NUM type guard;
- exit 1: limit NUM type guard;
- exit 2: inclusive ascending preheader comparison;
- exit 3: XPOLL for profile or STOPREQ work; and
- exit 4: final body comparison.

An integer accumulator `(1, 20.25, 2.0) -> 32.0` first takes exit 0; the
interpreter updates the accumulator to NUM, then the call re-enters and
finishes at exit 4. Integer factor `(0.5, 20.25, 2) -> 32.0` repeatedly takes
exit 0. Integer limit `(0.5, 20, 2.0) -> 32.0` repeatedly takes exit 1.
`(15.0, 20.25, 2.0) -> 30.0` repeatedly takes precondition exit 2. Repeated
hot exits leave `trace_nchild == 0` and `trace_nextside == 0`.

### Genuine NUM inputs and the conversion boundary

The positive C fixture sends all record and reuse values through
`lua_pushnumber`, including integral-looking factor `2.0`, so all three
admitted parameters are genuine NUM TValues. The deliberate integer cases use
`lua_pushinteger`.

Lua source spelling alone does not guarantee that distinction in this
dual-number runtime. A source literal such as `2.0` may be INT-tagged; using
it as a recorded factor introduces an INT-to-NUM `IR_CONV`, which the exact
constant-free profile rejects. This remains expected fail-closed behavior,
not a missing FMUL lowering.

## IEEE behavior and STOPREQ lifecycle

Profile and STOPREQ requests are published after root admission and frame
publication but before native SLOAD/FMUL execution. A profile request reaches
XPOLL exit 3, cleans up, re-enters the same root and finishes at exit 4.
STOPREQ reaches exit 3 with the expected VM-shutdown error, advances and
acknowledges the epoch, releases the handshake leader, drains pending,
request, poll and profile state, consumes STOPREQ freshness, restores the
idle VM state, native depth and pre-call C frame, and leaves the root reusable
after the sticky stop bit is explicitly cleared.

The fixture also replaces every live argument after admission and before its
native load or first FMUL. The baseline is the genuine-NUM tuple
`(0.5, 20.25, 2.0)`:

- `x = qNaN` returns NaN through precondition exit 2;
- `x = +Inf` returns positive infinity through exit 2;
- `x = -Inf` remains negative infinity under multiplication by two, so a
  simultaneous STOPREQ proves XPOLL exit 3;
- `x = +0` remains positive zero below the positive limit, so simultaneous
  STOPREQ proves exit 3;
- `limit = qNaN` and `limit = -Inf` return the first product, `1.0`, through
  exit 2;
- `limit = +Inf` eventually produces positive infinity, and inclusive
  `+Inf <= +Inf` remains true, so simultaneous STOPREQ proves exit 3;
- `factor = qNaN` returns NaN through exit 2;
- `factor = +Inf` returns positive infinity through exit 2;
- `factor = -Inf` makes the first product negative infinity and the body
  product positive infinity, then returns through final exit 4;
- `factor = +0` leaves the accumulator at positive zero indefinitely, so
  simultaneous STOPREQ proves exit 3;
- `factor = +1` leaves the accumulator unchanged, so simultaneous STOPREQ
  proves exit 3; and
- `factor = -1` oscillates between equal-magnitude signs below the positive
  limit, so simultaneous STOPREQ proves exit 3.

The `+0` accumulator and `-1` factor use dedicated post-admission request
types and raw IEEE-754 bit patterns. Their routing is scoped to argument slot
0 and slot 2 respectively and is pinned by the source contract. The
inclusive `limit = +Inf` STOPREQ path is separate from strict `MUL_LT`, where
the final strict `+Inf < +Inf` comparison exits instead.

Every exceptional or intentionally nonterminating mutation is followed by
complete handshake cleanup, exact-root revalidation, C-frame restoration and
finite exit-4 reuse. Admission adds no progress or finiteness check; these
tests prove that certified XPOLL preserves external interruption for source
programs that are intrinsically nonterminating.

## Native no-trace closure

Six new source identities remain explicitly interpreted:

- fixed initializer with live limit and factor;
- fixed factor with live accumulator and limit;
- reversed comparison, `while limit >= x`;
- reversed recurrence operands, `x = factor * x`;
- an extra recurrence, `x = x * factor * factor`; and
- adjacent division, `x = x / factor`.

These cases are finite in the fixture and each is followed by an exact
`expect_no_trace` check. The contract pins all six names exactly. Descending
strict and inclusive MUL (`>` and `>=`) remain interpreted, as do DIV roots,
fixed-initializer/fixed-constant arithmetic outside earlier certificates,
reversed ADD/SUB shapes, extra arithmetic and all other adjacent source
families.

The obsolete exact `MUL_LE` no-trace negative is forbidden from returning.
The already admitted strict `MUL_LT` profile remains positive and is rerun
unchanged. Repeated speculative exits do not create a child or side link, and
flush restores the original loop bytecode and removes the runnable root.

## Commits

The inclusive multiplication tranche was published incrementally through
`456f3b1b`:

- `5af75596` - admit exact semantic and post-RA `MUL_LE`, assign profile ID 8,
  and limit NUM MUL authorization to exact `MUL_LT`/`MUL_LE`;
- `4948dd31` - add the eight-profile synthetic mutation and 512-combination
  semantic/post-RA compiler certificate; and
- `456f3b1b` - add the ARM64/arm64e native runtime, exact mcode, equality,
  type-exit, IEEE, nontermination, lifecycle and no-trace closure proof, and
  update focused and umbrella success wording.

## Focused and complete validation

Confirmed focused validation for the runtime checkpoint completed on this
Apple Silicon host:

- scoped `git diff --check` and shell syntax checks passed;
- the ARM64 native fixture passed a direct `-Werror` syntax compilation;
- `tools/ci/arm64_jit_pure_numeric_args_contract.sh` completed an ordinary
  ARM64 direct run and two randomized-mcode runs;
- the same focused contract rebuilt for ARM64e/BTI and completed a direct run
  and two randomized-mcode runs;
- all six runtime executions reported
  `t-arm64-jit-pure-numeric-args OK`, including the exact instruction-word,
  lifecycle, exceptional-value and no-trace assertions; and
- the contract restored the ordinary experimental ARM64 build afterward.

The recurring ARM64 `ccall_rawchild_wait` unused-function warning appeared in
the builds and predates this tranche.

The complete `tools/ci/arm64_jit_fail_closed_gate.sh` umbrella also passed at
exact checkpoint `456f3b1b`. This includes execution of the focused synthetic
`tools/ci/arm64_jit_ir_admission_contract.sh` compiler contract. Its final
success banner includes the exact `MUL_LT` and `MUL_LE` FMUL profiles.

Independent isolated x86_64 validation also passed:

- the platform build and smoke check completed;
- the result was a thin Mach-O x86_64 binary;
- under Rosetta, `jit.os` reported `OSX`, `jit.arch` reported `x64`, and the
  smoke trace reported `trace=1`, `link=1`, `linktype=loop`; and
- the x86_64 stock suite completed all 509 tests.

That isolated run emitted only the expected discarded non-GC64 probe and the
known unused-`topofs` warning.

The native ARM64 stock suite also passed all 509 tests at exact checkpoint
`456f3b1b`.

Finally, eleven independent fresh processes each published exactly `trace=1`
with `link=1` and `linktype=loop` for:

- mixed-NUM;
- fixed-half;
- dynamic-step;
- `ADD_LT`;
- `ADD_LE`;
- `MUL_LT`, with a genuine NUM factor supplied by `math.sqrt(4)`;
- `MUL_LE`, with the same genuine NUM factor construction;
- `ADD_GT`;
- `ADD_GE`;
- `SUB_GT`; and
- `SUB_GE`.

These results complete the scoped focused, umbrella, native ARM64, fresh-
process and isolated x86_64 validation matrix for the `456f3b1b` tranche.

## Boundary

This tranche proves only the exact all-parameter inclusive ascending MUL
profile described above. It does not authorize fixed-initializer or
fixed-factor MUL, reversed operands, descending MUL, arbitrary ADD/SUB/MUL,
division, conversions, spills, calls, heap effects, side traces or stitches.
The positive factor is a property of terminating certificate tuples, not an
admission condition. The IEEE and zero/one/negative-factor proofs mutate
arguments after admission; they do not add an admission-time sign, finiteness
or progress check.

This tranche exercises the established lockless root-entry, publication,
polling, exit restoration and cleanup paths for one exact new shape. It does
not by itself complete or prove the full lockless ARM64 VM, FFI and JIT port;
that broader port remains incomplete.

## Next bounded tranche

The next distinct arithmetic candidate is the exact strict ascending DIV root:

```lua
while x < limit do
  x = x / divisor
end
```

Disposable-clone reconnaissance found the same 13-bytecode, ten-reference and
five-snapshot geometry, with exact `DIVVV A=3 B=3 C=4`,
`IR_DIV(X,DIVISOR)`, `GT`, `IR_DIV(X_PRE,DIVISOR)`, and `LT`. The existing
ARM64 backend already emits two direct FDIV instructions for this shape, but
the current production gate correctly rejects it. A production tranche should
therefore use a separate `ARM64_NUMDYN_DIV_LT` profile and `allow_num_div`
authorization, expand each compiler cross-product to 576 cases with exactly
nine admissions, and retain `DIV_LE`, reversed operands, fixed operands and
generic division as no-trace shapes. Division's signed-zero, infinity and
nontermination surface also requires its own runtime certificate rather than
being inferred from MUL.

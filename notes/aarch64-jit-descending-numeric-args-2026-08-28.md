# ARM64 JIT strict descending all-parameter pure-NUM root

Date: 2026-08-28

## Scope

This tranche admits one additional exact macOS ARM64 root while keeping the
broad recorder, side-trace and stitch gates closed:

```lua
local function f(x, limit, step)
  while x > limit do
    x = x - step
  end
  return x
end
```

The accumulator, limit and step are all live `NUM` parameters. This is a
strict descending `SUB_GT` profile over the same constant-free prototype,
snapshot and allocation geometry as the strict and inclusive ascending
all-parameter roots. It adds no spill rule, conversion, call path, heap
operation, side/stitch admission or publication mechanism. The existing ARM64
backend already lowers NUM `IR_SUB` to double-precision FSUB; the new work is
an exact admission and native-behavior certificate around that existing path.

## Exact recorder certificate

The admitted prototype is fixed to:

- `framesize = 5`, `sizebc = 13`, `numparams = 3`;
- no upvalues, numeric constants or GC constants;
- exactly `PROTO2_CELLOPS` in `flags2`; and
- root `startpc = proto_bc(pt) + 5`.

The native source-prototype check additionally observes
`flags == PROTO_HAS_RETURN`.

All thirteen bytecodes and their operand/jump fields are checked:

```text
0  FUNCF A=5
1  CGET  A=3 D=0
2  CGET  A=4 D=1
3  ISGE  A=4 D=3
4  JMP   A=3 +6
5  LOOP  A=3 +5
6  CGET  A=3 D=0
7  CGET  A=4 D=2
8  SUBVV A=3 B=3 C=4
9  CSET  A=0 D=3
10 JMP   A=3 -10
11 CGET  A=3 D=0
12 RET1  A=3 D=2
```

The reversed comparison operands at bytecode 3 and non-commutative SUB
operands at bytecode 8 are part of the profile. The live root changes bytecode
5 from `BC_LOOP` to `BC_JLOOP D=1`; flush must restore the trace-owned original
`BC_LOOP A=3 J=5`.

The semantic IR has no constants (`nk == REF_TRUE`) and exactly ten
instructions after `REF_BASE`:

1. guarded NUM SLOAD `X` from slot 2;
2. guarded NUM SLOAD `STEP` from slot 4;
3. PHI-marked `X_PRE = X - STEP`;
4. guarded NUM SLOAD `LIMIT` from slot 3;
5. guarded `LIMIT < X_PRE`;
6. `LOOP`;
7. `XPOLL 1`;
8. PHI-marked `X_BODY = X_PRE - STEP`;
9. guarded `X_BODY > LIMIT`; and
10. `PHI(X_PRE, X_BODY)`.

The `SUB_GT` tuple is therefore exact as
`BC_ISGE A=4 D=3 + IR_SUB(X,STEP) + IR_LT(LIMIT,X_PRE) +
IR_SUB(X_PRE,STEP) + IR_GT(X_BODY,LIMIT)`. It is not inferred from a generic
SUB or relational family.

The five snapshots are pinned to refs `X`, `LIMIT`, the preheader guard,
`LOOP`, and the body guard. Their map offsets are `{0,2,6,9,12}`, entry counts
are `{0,2,1,1,1}`, slot counts are `{5,6,5,5,5}`, `topslot` is 5, and footer
PCs are `{bc+6,bc+2,bc+11,bc+6,bc+11}`. There are exactly 15 snapshot-map
entries. The five restored entries are slots `{2,5,2,2,2}`, all with zero
flags, referencing `X_PRE` four times and `X_BODY` once. The base delta is
zero.

## Fail-closed production design

The all-parameter classifier now has three named full-shape profiles:
`ADD_LT`, `ADD_LE`, and `SUB_GT`. `arm64_numacc_grammar_profile()` acquires
both live prototype bytecode 3 and bytecode 8 and accepts only these pairs:

- `ISGE A=3 D=4` plus exact `ADDVV A=3 B=3 C=4` for `ADD_LT`;
- `ISGT A=3 D=4` plus exact `ADDVV A=3 B=3 C=4` for `ADD_LE`; or
- `ISGE A=4 D=3` plus exact `SUBVV A=3 B=3 C=4` for `SUB_GT`.

The semantic and independent post-RA passes each reacquire that grammar from
the prototype. Their shared dynamic-NUM kernel derives the recurrence opcode,
first-recurrence operand order, preheader guard and body guard from the exact
profile. The fixed-initializer dynamic-step root still calls the shared kernel
with `ADD_LT` explicitly and cannot inherit SUB admission.

The general scalar walk permits NUM `IR_SUB` only when the root is `BC_LOOP`
and the live all-parameter prototype has already classified as `SUB_GT`.
Every SUB producer must still have NUM type, exact operands, exact PHI flags
and exact position in the ten-reference shape. The post-RA walk repeats the
same restriction before checking layout. Thus adding the dedicated `IR_SUB`
case does not admit integer SUB, SUB in another numeric root, or arbitrary NUM
SUB elsewhere in a trace.

## Post-RA and native certificate

The post-RA certificate requires one terminal zeroed `NOP`, zero stack
adjustment, no spills or renames, and FPR-only numeric values. The
loop-carried `X_PRE/X_BODY/PHI` family must share one register. `STEP`, `LIMIT`
and that family must be pairwise distinct, and the original `X` cannot alias
`STEP`. `X` may alias `LIMIT` or the PHI family after its first SUB use dies.
Both first and body SUB operand directions are checked even after register
allocation.

The observed allocation is unchanged from the ascending profiles:
`X=d2`, `STEP=d1`, `X_PRE/X_BODY/PHI=d15`, and `LIMIT=d0`. Ordinary ARM64
emits exactly 136 bytes with `mcloop = 76`. ARM64e/BTI emits exactly 140 bytes
with `mcloop = 80` and a leading `BTI J`. Both forms contain exactly two
double-precision FSUBs, no FADD, and two double-precision FCMPs.

For both guards the emitted comparison is `FCMP LIMIT, PHI`. The preheader
branches `HS` to exit 2, so equality, a limit above the accumulator, and an
unordered NaN comparison leave native execution. The body branches `LO` back
to the loop and otherwise follows the unconditional exit-4 branch. Equality
therefore completes through exit 4, while unordered values cannot remain on
the native backedge. This is the ordered ARM64 realization of strict `>`; it
does not authorize the neighboring inclusive descending condition.

## Runtime and lifecycle proof

The native fixture records `(20.5, 0.25, 0.5) -> 0.0`, then reuses the same
root with the sensitive live-argument tuple
`(0.5, -0.625, 0.375) -> -0.625`. A stale recorded accumulator, limit or step
changes that result. Root count, prototype ownership, link type, IR,
snapshots, allocation and decoded machine code are revalidated after reuse.

Strict equality is covered at all three boundaries:

- `(1.0, 0.25, 0.375) -> 0.25` reaches equality at the body guard and exits
  through final exit 4;
- `(1.0, 0.5, 0.5) -> 0.5` reaches equality after the first SUB and exits
  through precondition exit 2; and
- `(0.5, 0.5, 0.5) -> 0.5` fails the interpreter's initial `>` and never
  enters the patched root.

All five exits are exercised while sides remain closed:

- exit 0: accumulator or step NUM type guard;
- exit 1: limit NUM type guard;
- exit 2: strict descending preheader comparison;
- exit 3: XPOLL for profile or STOPREQ work; and
- exit 4: final body comparison.

An integer accumulator demonstrates exit-0 interpreter conversion followed by
native re-entry. Integer step and limit inputs exercise their exact type exits,
and `(0.75, 0.5, 0.5) -> 0.25` exercises exit 2. Repeated hot exits leave
`trace_nchild == 0` and `trace_nextside == 0`.

Profile and STOPREQ requests are published at the post-admission boundary,
after the last entry-side request check and before native SLOAD/FSUB execution.
Profile work reaches XPOLL exit 3, cleans up, re-enters the same root and
finishes at exit 4. STOPREQ reaches exit 3 with the expected VM-shutdown error,
advances and acknowledges the epoch, releases the handshake leader, drains
pending/request/poll/profile state, consumes STOPREQ freshness, restores idle
VM/native-depth state and the pre-call C frame, and leaves the root reusable
after the sticky stop bit is explicitly cleared.

The descending fixture also replaces every live argument after admission and
before its native load or first SUB, using the finite baseline
`(20.25, 0.25, 0.5) -> 0.25`:

- `x = qNaN` exits at 2 and returns NaN;
- `x = +Inf` remains positive infinity under subtraction, so simultaneous
  STOPREQ proves safe termination through XPOLL exit 3;
- `x = -Inf` exits at 2 and returns negative infinity;
- `limit = qNaN` and `limit = +Inf` both exit at 2 and return `19.75`;
- `limit = -Inf` would keep the descending loop running, so simultaneous
  STOPREQ proves exit 3;
- `step = qNaN` exits at 2 and returns NaN;
- `step = +Inf` produces negative infinity, exits at 2 and returns negative
  infinity; and
- `step = -Inf` produces and retains positive infinity, so simultaneous
  STOPREQ proves exit 3.

Each mutation is followed by complete handshake cleanup, exact root
revalidation and a finite exit-4 reuse. The admission adds no step-sign or
finiteness guard: zero, negative and exceptional steps retain the source
program's possible nontermination, while the certified `XPOLL` keeps STOPREQ
available.

Trace flush restores the original `BC_LOOP` and removes the runnable root.

## Negative closure

The synthetic fixture exercises the complete
`3 profiles x 2 pre-arithmetic ops x 2 body-arithmetic ops x 3 pre-guards x
3 body-guards` product: 108 combinations at the semantic gate and the same
108 at post-RA. Exactly the coherent `ADD_LT`, `ADD_LE`, and `SUB_GT` rows are
admitted. Mixed guard/arithmetic profiles, reversed IR operands, live
comparison-bytecode operand reversal, live ADD/SUB bytecode replacement and
the coherent-but-unapproved descending-inclusive
`ISGT A=4 D=3 + SUB + LE/GE` profile all fail independently.

The existing exhaustive mutations are run for all three admitted profiles:
every semantic IR field, every prototype identity field, opcode/A/D-C/B bits
of all thirteen bytecodes, start PC/instruction, trace and constant bounds,
every snapshot header/restored entry/footer PC, `nsnap`, `nsnapmap`, `topslot`
and base delta. Post-RA repeats the semantic and snapshot corruption checks and
rejects spills, GPR or out-of-range registers, impossible aliases, split PHI
registers, nonzero stack adjustment, renames, malformed terminal NOPs and
extra or missing instructions.

Native negative cases keep fixed-initializer and fixed-half descending roots
interpreted. Descending `>=`, ascending `<=` with SUB, descending MUL or DIV,
reversed `limit < x`, reversed `step - x`, and an extra-SUB recurrence remain
closed. Existing ascending negatives continue to reject `>`/`>=` ADD roots,
reversed comparisons, reversed ADD, ascending SUB/MUL/DIV and extra ADD.

## Commits and validation

The tranche was published incrementally through `e949acbb`:

- `221a6885` - admit the exact semantic and post-RA `SUB_GT` profile;
- `62295769` - add the three-profile synthetic mutation, allocation and
  108-combination certificate;
- `cee270ee` - add the ARM64/arm64e native runtime, lifecycle and exceptional
  value proof; and
- `e949acbb` - name the descending root in the umbrella gate summary.

Validation completed on this Apple Silicon host:

- the focused semantic/post-RA admission contract, including native ARM64
  fixture execution and arm64e/BTI `-Werror` source compilation;
- the focused runtime contract in six configurations: direct plus two
  randomized-mcode executions on ordinary ARM64, and direct plus two
  randomized-mcode executions on ARM64e/BTI; and
- the complete `tools/ci/arm64_jit_fail_closed_gate.sh` umbrella with the new
  semantic and runtime certificates integrated;
- the native ARM64 stock suite: 509 passed;
- the thin macOS x86_64 platform build and smoke, a real Rosetta JIT loop with
  `jit.os == "OSX"`, `jit.arch == "x64"`, and `linktype == "loop"`, followed
  by the x86_64 stock suite: 509 passed; and
- restoration of the exact ordinary ARM64 experimental helper build followed
  by live publication of the fixed-half, dynamic-step, strict all-parameter,
  inclusive all-parameter, descending all-parameter and mixed-NUM roots, all
  with `linktype == "loop"`.

The x86_64 platform builder's discarded preliminary non-GC64 clean diagnostic
and unused `topofs` warning are expected and pre-existing; the actual target
build, smoke and 509-test suite passed. The recurring ARM64 unused
`ccall_rawchild_wait` warning is also pre-existing.

## Boundary after this tranche

This admission proves only the exact all-parameter strict descending SUB
profile described above. It does not authorize descending-inclusive roots,
fixed-initializer subtraction, reversed operands, arbitrary SUB, another
relational family, conversions, spills, calls, heap effects, side traces or
stitches. Any further widening needs its own bytecode, semantic IR, snapshot,
post-RA, machine-code and exceptional-value certificate.

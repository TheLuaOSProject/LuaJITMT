# ARM64 JIT inclusive descending all-parameter pure-NUM root

Date: 2026-08-28

## Scope

This tranche admits one additional exact macOS ARM64 root while keeping the
broad recorder, side-trace and stitch gates closed:

```lua
local function f(x, limit, step)
  while x >= limit do
    x = x - step
  end
  return x
end
```

The accumulator, limit and step are all live `NUM` parameters. This is the
inclusive descending `SUB_GE` counterpart to the already-certified strict
descending root. It uses the same constant-free prototype, snapshot and
allocation geometry as the strict/inclusive ascending and strict descending
all-parameter roots. It adds no new backend opcode, spill rule, conversion,
call path, heap operation, side/stitch admission or publication mechanism.
The existing ARM64 backend already lowers NUM `IR_SUB` to double-precision
FSUB; this work adds only an exact admission and native-behavior certificate
for the inclusive source grammar.

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
3  ISGT  A=4 D=3
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

The comparison operands at bytecode 3 and the non-commutative SUB operands at
bytecode 8 are part of the profile. The live root changes bytecode 5 from
`BC_LOOP` to `BC_JLOOP D=1`; flush must restore the trace-owned original
`BC_LOOP A=3 J=5`.

The semantic IR has no constants (`nk == REF_TRUE`) and exactly ten
instructions after `REF_BASE`:

1. guarded NUM SLOAD `X` from slot 2;
2. guarded NUM SLOAD `STEP` from slot 4;
3. PHI-marked `X_PRE = X - STEP`;
4. guarded NUM SLOAD `LIMIT` from slot 3;
5. guarded `LIMIT <= X_PRE`;
6. `LOOP`;
7. `XPOLL 1`;
8. PHI-marked `X_BODY = X_PRE - STEP`;
9. guarded `X_BODY >= LIMIT`; and
10. `PHI(X_PRE, X_BODY)`.

The exact tuple is therefore
`BC_ISGT A=4 D=3 + IR_SUB(X,STEP) + IR_LE(LIMIT,X_PRE) +
IR_SUB(X_PRE,STEP) + IR_GE(X_BODY,LIMIT)`. It is not inferred from a
generic SUB or relational family.

The five snapshots are pinned to refs `X`, `LIMIT`, the preheader guard,
`LOOP`, and the body guard. Their map offsets are `{0,2,6,9,12}`, entry counts
are `{0,2,1,1,1}`, slot counts are `{5,6,5,5,5}`, `topslot` is 5, and footer
PCs are `{bc+6,bc+2,bc+11,bc+6,bc+11}`. There are exactly 15 snapshot-map
entries. The five restored entries are slots `{2,5,2,2,2}`, all with zero
flags, referencing `X_PRE` four times and `X_BODY` once. The base delta is
zero.

## Fail-closed production design

The all-parameter classifier now has four named full-shape profiles:

- `ADD_LT`: `ISGE A=3 D=4`, exact `ADDVV A=3 B=3 C=4`, pre/body `GT/LT`;
- `ADD_LE`: `ISGT A=3 D=4`, exact `ADDVV A=3 B=3 C=4`, pre/body `GE/LE`;
- `SUB_GT`: `ISGE A=4 D=3`, exact `SUBVV A=3 B=3 C=4`, pre/body `LT/GT`;
- `SUB_GE`: `ISGT A=4 D=3`, exact `SUBVV A=3 B=3 C=4`, pre/body `LE/GE`.

`arm64_numacc_grammar_profile()` acquires the live comparison and recurrence
bytecodes and returns only one of those exact rows. The semantic and
independent post-RA kernels derive recurrence opcode, first-recurrence operand
order and both guards from the selected row. A dedicated helper identifies
only `SUB_GT` and `SUB_GE` as SUB profiles.

The general scalar walk therefore permits NUM `IR_SUB` only when the root is
`BC_LOOP` and the live all-parameter prototype has already classified as one
of those two exact descending profiles. Every SUB producer must still have
NUM type, exact operands, exact PHI flags and the exact position in the
ten-reference shape. Post-RA repeats the restriction before checking layout.
The fixed-initializer dynamic-step root remains explicitly pinned to
`ADD_LT`, so it cannot inherit inclusive or SUB admission.

The source contract scopes the complete seven-line `SUB_GE` tuple separately
inside both kernels and scopes the exact `SUBVV A=3 B=3 C=4` selector arm with
both `A=4,D=3` comparison rows. It also refuses to link a stale assembler:
`src/lj_asm.o` must be newer than `src/lj_asm.c`, and the archive member must
be byte-identical to that object. Its arm64e audit explicitly enables BTI,
verifies `LJ_TARGET_ARM64`, `LJ_ABI_PAUTH` and `LJ_ABI_BRANCH_TRACK`, then
compiles the current assembler with `-Werror`.

## Post-RA and native certificate

The post-RA certificate requires a zeroed terminal `NOP`, zero stack
adjustment, no spills or renames, and FPR-only numeric values. The
loop-carried `X_PRE/X_BODY/PHI` family shares one register. `STEP`, `LIMIT`
and that family are pairwise distinct, and the original `X` cannot alias
`STEP`. Both first and body SUB operand directions are rechecked after
allocation.

The observed allocation is `X=d2`, `STEP=d1`,
`X_PRE/X_BODY/PHI=d15`, and `LIMIT=d0`. Ordinary ARM64 emits exactly 136
bytes with `mcloop = 76`. ARM64e/BTI emits exactly 140 bytes with
`mcloop = 80` and a leading `BTI J`. Both forms contain exactly two
double-precision FSUBs, no FADD, and two double-precision FCMPs.

Both comparisons are `FCMP LIMIT, PHI`. The preheader branches `HI` to exit 2,
so a limit strictly above the accumulator or an unordered NaN comparison
leaves native execution, while equality remains admitted. The body branches
`LS` back to the loop and otherwise follows the unconditional exit-4 branch.
Equality therefore takes one more inclusive iteration; unordered values
cannot remain on the native backedge.

## Runtime and lifecycle proof

The native fixture records `(20.5, 0.25, 0.5) -> 0.0`, then reuses the same
root with the exact-binary live-argument tuple
`(0.375, -0.625, 0.25) -> -0.875`. Replacing only `x`, `limit`, or `step`
with its recorded value would instead produce `-0.75`, `0.125`, or `-1.125`,
so the result detects stale specialization of every parameter. Root count,
prototype ownership, link type, IR, snapshots, allocation and decoded machine
code are revalidated after reuse.

Inclusive equality is covered at all three boundaries:

- `(1.0, 0.25, 0.375) -> -0.125` reaches equality at the body guard, takes
  one more backedge and completes through final exit 4;
- `(1.0, 0.5, 0.5) -> 0.0` reaches equality after the first SUB, passes the
  inclusive precondition and completes through exit 4; and
- `(0.5, 0.5, 0.5) -> 0.0` enters from initial equality, then its first SUB
  falls below the limit and leaves through precondition exit 2.

All five exits are exercised while sides remain closed:

- exit 0: accumulator or step NUM type guard;
- exit 1: limit NUM type guard;
- exit 2: inclusive descending preheader comparison;
- exit 3: XPOLL for profile or STOPREQ work; and
- exit 4: final body comparison.

Integer accumulator, step and limit inputs exercise their exact type exits.
An integer accumulator demonstrates interpreter conversion followed by native
re-entry; repeated hot exits leave `trace_nchild == 0` and
`trace_nextside == 0`. `(0.75, 0.5, 0.5) -> 0.25` exercises the precondition
exit. Flush restores the original `BC_LOOP` and removes the runnable root.

Profile and STOPREQ requests are published after admission and before native
SLOAD/FSUB execution. Profile work reaches XPOLL exit 3, cleans up, re-enters
the same root and finishes at exit 4. STOPREQ reaches exit 3 with the expected
VM-shutdown error, advances and acknowledges the epoch, releases the handshake
leader, drains pending/request/poll/profile state, consumes STOPREQ freshness,
restores idle VM/native-depth state and the pre-call C frame, and leaves the
root reusable after the sticky stop bit is explicitly cleared.

The fixture also replaces every live argument after admission and before its
native load or first SUB, using the finite baseline
`(20.25, 0.25, 0.5) -> -0.25`:

- `x = qNaN` exits at 2 and returns NaN;
- `x = +Inf` remains positive infinity, so simultaneous STOPREQ proves safe
  termination through XPOLL exit 3;
- `x = -Inf` exits at 2 and returns negative infinity;
- `limit = qNaN` and `limit = +Inf` exit at 2 and return `19.75`;
- `limit = -Inf` would keep the loop running, so simultaneous STOPREQ proves
  exit 3;
- `step = qNaN` exits at 2 and returns NaN;
- `step = +Inf` produces negative infinity, exits at 2 and returns it; and
- `step = -Inf` produces and retains positive infinity, so simultaneous
  STOPREQ proves exit 3.

Each mutation is followed by complete handshake cleanup, exact root
revalidation, C-frame restoration and finite exit-4 reuse. The admission adds
no step-sign or finiteness guard: zero, negative and exceptional steps retain
the source program's possible nontermination, while the certified `XPOLL`
keeps STOPREQ available.

## Negative closure

The synthetic fixture exercises the complete
`4 profiles x 2 pre-arithmetic ops x 2 body-arithmetic ops x 4 pre-guards x
4 body-guards` product: 256 combinations at the semantic gate and the same
256 at post-RA. Exactly the four coherent named rows are admitted. Mixed
guard/arithmetic profiles, reversed IR operands, live comparison-bytecode
strictness/operand changes, live ADD/SUB recurrence changes and every adjacent
opcode remain closed independently.

The exhaustive semantic, prototype, bytecode, snapshot and post-RA mutation
suites run for all four profiles. They reject every changed IR field,
prototype identity field, bytecode opcode/operand/jump field, trace bound,
snapshot header/restored value/footer PC, spill, register-class violation,
impossible alias, split PHI register, nonzero stack adjustment, rename,
malformed terminal NOP, and extra or missing instruction.

Native negatives keep fixed-initializer and fixed-half inclusive-descending
roots interpreted. An ascending `<=` SUB root, a descending-inclusive `>=`
ADD root, descending-inclusive MUL or DIV, reversed `limit <= x`, reversed
`step - x`, and an extra-SUB recurrence all publish no trace. The contract
also pins the exact `main()` invocation of every fixed and negative suite, so
these proof bodies cannot silently become dead code.

## Commits and validation

The tranche was published incrementally through `cf73fdb9`:

- `7d44abde` - admit the exact semantic and post-RA `SUB_GE` profile;
- `dfa144f7` - add the four-profile synthetic mutation, allocation and
  256-combination certificate;
- `d266588b` - add the ARM64/arm64e runtime, lifecycle and exceptional-value
  proof; and
- `cf73fdb9` - name `SUB_GE` in the umbrella gate summary.

Independent read-only audits found and closed stale-archive, incomplete-BTI,
scoped static-proof and dead-suite-invocation gaps before the test commits.

Validation completed on this Apple Silicon host:

- the focused semantic/post-RA admission contract, including native ARM64
  execution, current-object/archive identity and arm64e/BTI `-Werror` source
  compilation;
- the focused runtime contract in six configurations: direct plus two
  randomized-mcode executions on ordinary ARM64, and direct plus two
  randomized-mcode executions on ARM64e/BTI;
- the complete `tools/ci/arm64_jit_fail_closed_gate.sh` umbrella with all
  prior root, first-side, publication, exit, retirement, GDBJIT, callback,
  flush/reuse and safepoint contracts;
- the native ARM64 JIT-enabled vendored suite: 509 passed;
- the thin macOS x86_64 platform build and smoke, a real Rosetta JIT loop with
  `jit.os == "OSX"`, `jit.arch == "x64"`, and `linktype == "loop"`, followed
  by the x86_64 vendored suite: 509 passed; and
- restoration of the exact ordinary ARM64 experimental helper build followed
  by live publication of the mixed-NUM, fixed-half, dynamic-step, `ADD_LT`,
  `ADD_LE`, `SUB_GT`, and `SUB_GE` roots, all with `linktype == "loop"`.

The x86_64 platform builder's discarded preliminary non-GC64 diagnostic and
unused `topofs` warning are expected and pre-existing; the actual target build,
smoke and 509-test suite passed. The recurring ARM64 unused
`ccall_rawchild_wait` warning is also pre-existing.

## Boundary and next bounded tranche

This admission proves only the exact all-parameter inclusive descending SUB
profile described above. It does not authorize fixed-initializer inclusive
loops, reversed operands, arbitrary ADD/SUB, multiplication, division,
conversions, spills, calls, heap effects, side traces or stitches.

The smallest useful next widening is the exact all-parameter strict
descending guard with an ADD recurrence and a negative live step:

```lua
while x > limit do
  x = x + step
end
```

That `ADD_GT` root reuses the already-certified ADD opcode and strict
descending guard family without widening the generic opcode allowlist. It
still needs its own source bytecode, semantic IR, snapshot, post-RA,
machine-code, lifecycle and exceptional-value certificate.

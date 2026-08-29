# ARM64 JIT strict descending ADD all-parameter pure-NUM root

Date: 2026-08-28

## Scope

This tranche admits one additional exact macOS ARM64 root while keeping the
broad recorder, side-trace and stitch gates closed:

```lua
local function f(x, limit, step)
  while x > limit do
    x = x + step
  end
  return x
end
```

The accumulator, limit and step are all live `NUM` parameters. This is the
strict `ADD_GT` grammar: the guard is descending and the certified terminating
tuples use a negative live step, but admission deliberately adds no step-sign
or finiteness guard. It uses the same constant-free prototype, snapshot and
allocation geometry as the strict/inclusive ascending and descending
all-parameter roots. It adds no backend opcode, spill rule, conversion, call
path, heap operation, side/stitch admission or publication mechanism. The
existing ARM64 backend already lowers NUM `IR_ADD` to double-precision FADD;
this work adds only an exact source-grammar admission and native-behavior
certificate.

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
8  ADDVV A=3 B=3 C=4
9  CSET  A=0 D=3
10 JMP   A=3 -10
11 CGET  A=3 D=0
12 RET1  A=3 D=2
```

The comparison operand direction at bytecode 3 and ADD operand order at
bytecode 8 are part of the profile even though ADD is commutative. The live
root changes bytecode 5 from `BC_LOOP` to `BC_JLOOP D=1`; flush must restore
the trace-owned original `BC_LOOP A=3 J=5`.

The semantic IR has no constants (`nk == REF_TRUE`) and exactly ten
instructions after `REF_BASE`:

1. guarded NUM SLOAD `X` from slot 2;
2. guarded NUM SLOAD `STEP` from slot 4;
3. PHI-marked `X_PRE = STEP + X`;
4. guarded NUM SLOAD `LIMIT` from slot 3;
5. guarded `LIMIT < X_PRE`;
6. `LOOP`;
7. `XPOLL 1`;
8. PHI-marked `X_BODY = X_PRE + STEP`;
9. guarded `X_BODY > LIMIT`; and
10. `PHI(X_PRE, X_BODY)`.

The exact tuple is therefore
`BC_ISGE A=4 D=3 + IR_ADD(STEP,X) + IR_LT(LIMIT,X_PRE) +
IR_ADD(X_PRE,STEP) + IR_GT(X_BODY,LIMIT)`. It is not inferred from a
generic ADD or relational family.

The five snapshots are pinned to refs `X`, `LIMIT`, the preheader guard,
`LOOP`, and the body guard. Their map offsets are `{0,2,6,9,12}`, entry counts
are `{0,2,1,1,1}`, slot counts are `{5,6,5,5,5}`, `topslot` is 5, and footer
PCs are `{bc+6,bc+2,bc+11,bc+6,bc+11}`. There are exactly 15 snapshot-map
entries. The five restored entries are slots `{2,5,2,2,2}`, all with zero
flags, referencing `X_PRE` four times and `X_BODY` once. The base delta is
zero.

## Fail-closed production design

The all-parameter classifier now has five named full-shape profiles:

- `ADD_LT`: `ISGE A=3 D=4`, exact `ADDVV A=3 B=3 C=4`, pre/body `GT/LT`;
- `ADD_LE`: `ISGT A=3 D=4`, exact `ADDVV A=3 B=3 C=4`, pre/body `GE/LE`;
- `ADD_GT`: `ISGE A=4 D=3`, exact `ADDVV A=3 B=3 C=4`, pre/body `LT/GT`;
- `SUB_GT`: `ISGE A=4 D=3`, exact `SUBVV A=3 B=3 C=4`, pre/body `LT/GT`;
  and
- `SUB_GE`: `ISGT A=4 D=3`, exact `SUBVV A=3 B=3 C=4`, pre/body `LE/GE`.

`arm64_numacc_grammar_profile()` acquires the live comparison and recurrence
bytecodes and returns only one of those exact rows. The semantic and
independent post-RA kernels derive recurrence opcode, first-recurrence operand
order and both guards from the selected row. The helper that authorizes NUM
SUB identifies only `SUB_GT` and `SUB_GE`; `ADD_GT` cannot expand that
authorization. The fixed-initializer dynamic-step root remains explicitly
pinned to `ADD_LT` and cannot inherit the new profile.

No generic arithmetic allowlist was widened. Every ADD producer must still
have NUM type, exact operands, exact PHI flags and the exact position in the
ten-reference shape. Post-RA repeats the restriction before checking layout.

The source contract scopes the complete `ADD_GT` tuple separately inside both
kernels and scopes its exact `ADDVV A=3 B=3 C=4` selector arm. It proves that
`ADD_GT` is absent from the SUB-authorization helper. It also refuses to link
a stale assembler: `src/lj_asm.o` must be newer than `src/lj_asm.c`, and the
archive member must be byte-identical to that object. Its arm64e audit
explicitly enables BTI, verifies `LJ_TARGET_ARM64`, `LJ_ABI_PAUTH` and
`LJ_ABI_BRANCH_TRACK`, then compiles the current assembler with `-Werror`.

## Post-RA and native certificate

The post-RA certificate requires a zeroed terminal `NOP`, zero stack
adjustment, no spills or renames, and FPR-only numeric values. The
loop-carried `X_PRE/X_BODY/PHI` family shares one register. `STEP`, `LIMIT`
and that family are pairwise distinct, and the original `X` cannot alias
`STEP`. The first and body ADD operands are rechecked after allocation.

The observed allocation is `X=d2`, `STEP=d1`,
`X_PRE/X_BODY/PHI=d15`, and `LIMIT=d0`. Ordinary ARM64 emits exactly 136
bytes with `mcloop = 76`. ARM64e/BTI emits exactly 140 bytes with
`mcloop = 80` and a leading `BTI J`. Both forms contain exactly two
double-precision FADDs, no FSUB, and two double-precision FCMPs.

Both comparisons are `FCMP LIMIT, PHI`. The preheader branches `HS` to exit 2,
so equality, a limit above the accumulator, and an unordered NaN comparison
leave native execution. The body branches `LO` back to the loop and otherwise
follows the unconditional exit-4 branch. Equality therefore completes through
exit 4, while unordered values cannot remain on the native backedge. This is
the ordered ARM64 realization of strict `>`; it does not authorize the
neighboring inclusive descending condition.

## Runtime and lifecycle proof

The native fixture records `(20.5, 0.25, -0.5) -> 0.0`, then reuses the same
root with the exact-binary live-argument tuple
`(0.5, -0.625, -0.375) -> -0.625`. Replacing only `x`, `limit`, or `step`
with its recording value would instead produce `-0.875`, `0.125`, or `-1.0`.
Those three stale-value calls leave through exits 4, 2 and 4 respectively, so
the results and exit paths detect specialization of every parameter. Root
count, prototype ownership, link type, IR, snapshots, allocation and decoded
machine code are revalidated after every reuse.

Strict equality is covered at all three boundaries:

- `(1.0, 0.25, -0.375) -> 0.25` reaches equality at the body guard and exits
  through final exit 4;
- `(1.0, 0.5, -0.5) -> 0.5` reaches equality after the first ADD and exits
  through precondition exit 2; and
- `(0.5, 0.5, -0.5) -> 0.5` fails the interpreter's initial `>` and never
  enters the patched root.

All five exits are exercised while sides remain closed:

- exit 0: accumulator or step NUM type guard;
- exit 1: limit NUM type guard;
- exit 2: strict descending preheader comparison;
- exit 3: XPOLL for profile or STOPREQ work; and
- exit 4: final body comparison.

An integer accumulator demonstrates exit-0 interpreter conversion followed by
native re-entry. Integer step and limit inputs exercise their exact type exits,
and `(0.75, 0.5, -0.5) -> 0.25` exercises exit 2. Repeated hot exits leave
`trace_nchild == 0` and `trace_nextside == 0`.

Profile and STOPREQ requests are published after admission and before native
SLOAD/FADD execution. Profile work reaches XPOLL exit 3, cleans up, re-enters
the same root and finishes at exit 4. STOPREQ reaches exit 3 with the expected
VM-shutdown error, advances and acknowledges the epoch, releases the handshake
leader, drains pending/request/poll/profile state, consumes STOPREQ freshness,
restores idle VM/native-depth state and the pre-call C frame, and leaves the
root reusable after the sticky stop bit is explicitly cleared.

The fixture also replaces every live argument after admission and before its
native load or first ADD, using the finite baseline
`(20.25, 0.25, -0.5) -> 0.25`:

- `x = qNaN` exits at 2 and returns NaN;
- `x = +Inf` remains positive infinity, so simultaneous STOPREQ proves safe
  termination through XPOLL exit 3;
- `x = -Inf` exits at 2 and returns negative infinity;
- `limit = qNaN` and `limit = +Inf` exit at 2 and return `19.75`;
- `limit = -Inf` would keep the loop running, so simultaneous STOPREQ proves
  exit 3;
- `step = qNaN` exits at 2 and returns NaN;
- `step = +Inf` produces and retains positive infinity, so simultaneous
  STOPREQ proves exit 3; and
- `step = -Inf` produces negative infinity, exits at 2 and returns it.

Each mutation is followed by complete handshake cleanup, exact root
revalidation, C-frame balance and finite exit-4 reuse. The admission adds no
step-sign or finiteness guard: zero, positive and exceptional steps retain the
source program's possible nontermination, while the certified `XPOLL` keeps
STOPREQ available. Trace flush restores the original `BC_LOOP` and removes the
runnable root.

## Negative closure

The synthetic fixture exercises the complete
`5 profiles x 2 pre-arithmetic ops x 2 body-arithmetic ops x 4 pre-guards x
4 body-guards` product: 320 combinations at the semantic gate and the same
320 at post-RA. Exactly the five coherent named rows are admitted. Mixed
guard/arithmetic profiles, reversed IR operands, live comparison-bytecode
strictness/operand changes, live ADD/SUB recurrence changes and every adjacent
opcode remain closed independently.

The exhaustive semantic, prototype, bytecode, snapshot and post-RA mutation
suites run for all five profiles. They reject every changed IR field,
prototype identity field, bytecode opcode/operand/jump field, trace bound,
snapshot header/restored value/footer PC, spill, register-class violation,
impossible alias, split PHI register, nonzero stack adjustment, rename,
malformed terminal NOP, and extra or missing instruction.

Native negatives keep fixed-initializer and fixed-half `ADD_GT` roots
interpreted. An inclusive descending `ADD_GE` root, reversed `limit < x`,
reversed `step + x`, an extra-ADD recurrence, and descending ADD with MUL or
DIV all publish no trace. Existing ascending and SUB negatives remain live.
The contract pins the exact `main()` invocation of all five positive profiles
and every negative suite, plus every no-trace identity, so those proof bodies
cannot silently become dead code.

## Commits and validation

The tranche was published incrementally through `edeb0e6c`:

- `fee967fb` - admit the exact semantic and post-RA `ADD_GT` profile;
- `4d4703ec` - add the five-profile synthetic mutation, allocation and
  320-combination certificate;
- `67c5c36b` - add the ARM64/arm64e native runtime, lifecycle and exceptional
  value proof; and
- `edeb0e6c` - name `ADD_GT` in the umbrella gate summary.

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
- the native ARM64 stock suite: 509 passed;
- restoration of the exact ordinary ARM64 experimental helper with its
  assembler object current and archive-identical, `LJ_ABI_PAUTH == 0`,
  `LJ_ABI_BRANCH_TRACK == 0`, root recording and LOOP native entry open, and
  the side recorder closed;
- fresh-process live publication of the mixed-NUM, fixed-half, dynamic-step,
  `ADD_LT`, `ADD_LE`, `ADD_GT`, `SUB_GT`, and `SUB_GE` roots, each with
  `trace == 1` and `linktype == "loop"`; and
- from a disposable checkout at exact commit `edeb0e6c`, the thin macOS
  x86_64 platform build and smoke, a real Rosetta JIT loop with
  `jit.os == "OSX"`, `jit.arch == "x64"`, and `linktype == "loop"`, followed
  by the x86_64 stock suite: 509 passed.

The x86_64 platform builder's discarded preliminary non-GC64 diagnostic and
unused `topofs` warning are expected and pre-existing; the actual target
build, smoke and 509-test suite passed. The recurring ARM64 unused
`ccall_rawchild_wait` warning is also pre-existing.

## Boundary and next bounded tranche

This admission proves only the exact all-parameter strict `ADD_GT` profile
described above. It does not authorize fixed-initializer descending ADD,
inclusive `ADD_GE`, reversed operands, arbitrary ADD/SUB, multiplication,
division, conversions, spills, calls, heap effects, side traces or stitches.

The smallest useful next widening is the exact all-parameter inclusive
descending guard with an ADD recurrence and a negative live step:

```lua
while x >= limit do
  x = x + step
end
```

That `ADD_GE` root is currently an explicit no-trace negative. It reuses the
already-certified ADD opcode and inclusive descending guard family, but still
needs its own source bytecode, semantic IR, snapshot, post-RA, machine-code,
lifecycle and exceptional-value certificate.

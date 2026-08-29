# ARM64 JIT strict ascending MUL all-parameter pure-NUM root

Date: 2026-08-28

## Scope

This tranche admits one additional exact macOS ARM64 root while keeping the
broad recorder, side-trace and stitch gates closed:

```lua
local function f(x, limit, factor)
  while x < limit do
    x = x * factor
  end
  return x
end
```

The accumulator, limit and factor are all live `NUM` parameters. This is the
strict ascending `MUL_LT` counterpart to the six already-certified
all-parameter ADD and SUB roots. The terminating certificate tuples use a
positive factor greater than one, but admission adds no factor-sign,
magnitude or finiteness guard. Factors zero or one and some exceptional
inputs preserve the source program's intrinsic nontermination.

The existing ARM64 backend already lowers NUM `IR_MUL` to double-precision
FMUL. This work adds an exact source-grammar admission, a separately bounded
MUL authorization and semantic, post-RA, native-behavior and lifecycle
certificates. It adds no backend opcode, spill rule, conversion, call path,
heap operation, side/stitch admission or publication mechanism.

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
3  ISGE  A=3 D=4
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

The comparison operand direction at bytecode 3 and MUL operand order at
bytecode 8 are part of the profile even though MUL is commutative. The live
root changes bytecode 5 from `BC_LOOP` to `BC_JLOOP D=1`; flush must restore
the trace-owned original `BC_LOOP A=3 J=5`.

The semantic IR has no constants (`nk == REF_TRUE`) and exactly ten
instructions after `REF_BASE`:

1. guarded NUM SLOAD `X` from slot 2;
2. guarded NUM SLOAD `FACTOR` from slot 4;
3. PHI-marked `X_PRE = FACTOR * X`;
4. guarded NUM SLOAD `LIMIT` from slot 3;
5. guarded `LIMIT > X_PRE`;
6. `LOOP`;
7. `XPOLL 1`;
8. PHI-marked `X_BODY = X_PRE * FACTOR`;
9. guarded `X_BODY < LIMIT`; and
10. `PHI(X_PRE, X_BODY)`.

The exact tuple is therefore
`BC_ISGE A=3 D=4 + IR_MUL(FACTOR,X) + IR_GT(LIMIT,X_PRE) +
IR_MUL(X_PRE,FACTOR) + IR_LT(X_BODY,LIMIT)`. It is not inferred from a
generic arithmetic, multiplication or relational family.

The five snapshots are pinned to refs `X`, `LIMIT`, the preheader guard,
`LOOP`, and the body guard. Their map offsets are `{0,2,6,9,12}`, entry counts
are `{0,2,1,1,1}`, slot counts are `{5,6,5,5,5}`, `topslot` is 5, and footer
PCs are `{bc+6,bc+2,bc+11,bc+6,bc+11}`. There are exactly 15 snapshot-map
entries. The five restored entries are slots `{2,5,2,2,2}`, all with zero
flags, referencing `X_PRE` four times and `X_BODY` once. The base delta is
zero.

## Fail-closed production design

The all-parameter classifier now has seven named full-shape profiles:

- `ADD_LT`: `ISGE A=3 D=4`, exact `ADDVV A=3 B=3 C=4`, pre/body `GT/LT`;
- `ADD_LE`: `ISGT A=3 D=4`, exact `ADDVV A=3 B=3 C=4`, pre/body `GE/LE`;
- `ADD_GT`: `ISGE A=4 D=3`, exact `ADDVV A=3 B=3 C=4`, pre/body `LT/GT`;
- `ADD_GE`: `ISGT A=4 D=3`, exact `ADDVV A=3 B=3 C=4`, pre/body `LE/GE`;
- `SUB_GT`: `ISGE A=4 D=3`, exact `SUBVV A=3 B=3 C=4`, pre/body `LT/GT`;
- `SUB_GE`: `ISGT A=4 D=3`, exact `SUBVV A=3 B=3 C=4`, pre/body `LE/GE`;
  and
- `MUL_LT`: `ISGE A=3 D=4`, exact `MULVV A=3 B=3 C=4`, pre/body `GT/LT`.

`arm64_numacc_grammar_profile()` acquires the live comparison and recurrence
bytecodes and returns only one exact row. MUL is not folded into the ADD or
SUB families: `arm64_numdynamic_is_mul()` returns true only for
`ARM64_NUMDYN_MUL_LT`, while the independent SUB helper still returns true
only for `SUB_GT` and `SUB_GE`.

Semantic and post-RA admission derive `allow_num_mul` only from that exact
MUL profile. Their dedicated `IR_MUL` cases then require a LOOP root, NUM
type, exact PHI flags and operands accepted by the profile-specific kernel.
The generic NUM producer helper accepts MUL only when that separate bit is
set. Integer `IR_MUL`, `IR_MULOV`, MUL in another NUM root, conversions and
all other arithmetic remain outside this authorization.

Both independent kernels derive recurrence opcode, first-recurrence operand
order and guards from the selected row. The source contract scopes the
complete `MUL_LT` tuple separately through the end of each kernel, pins the
one exact classifier branch and proves that MUL authorization excludes every
non-MUL profile. The fixed-initializer dynamic-step root remains explicitly
pinned to `ADD_LT`.

No generic arithmetic allowlist was widened. Every admitted MUL producer must
have NUM type, exact operands, exact PHI flags and the exact position in the
ten-reference shape. Post-RA repeats the same restriction before checking
layout.

The admission contract refuses to link a stale assembler: `src/lj_asm.o` must
be newer than `src/lj_asm.c`, and the archive member must be byte-identical to
that object. Its arm64e audit explicitly enables BTI, verifies
`LJ_TARGET_ARM64`, `LJ_ABI_PAUTH` and `LJ_ABI_BRANCH_TRACK`, then compiles the
current assembler with `-Werror`.

## Post-RA and native certificate

The post-RA certificate requires a zeroed terminal `NOP`, zero stack
adjustment, no spills or renames, and FPR-only numeric values. The
loop-carried `X_PRE/X_BODY/PHI` family shares one register. `FACTOR`, `LIMIT`
and that family are pairwise distinct, and the original `X` cannot alias
`FACTOR`. Both first and body MUL operand sets are rechecked after allocation.

The observed allocation is `X=d2`, `FACTOR=d1`,
`X_PRE/X_BODY/PHI=d15`, and `LIMIT=d0`. Ordinary ARM64 emits exactly 136
bytes with `mcloop = 76`. ARM64e/BTI emits exactly 140 bytes with
`mcloop = 80` and a leading `BTI J`. Both forms contain exactly two
double-precision FMULs, no FADD or FSUB, and two double-precision FCMPs.

The first recurrence is `FMUL d15,d1,d2`; the loop body is
`FMUL d15,d15,d1`. Both comparisons are `FCMP d15,d0`. The preheader branches
`HS` to exit 2, so an accumulator greater than or equal to the limit, or an
unordered NaN comparison, leaves native execution. The body branches `LO`
back to the loop and otherwise follows the unconditional exit-4 branch.
Equality therefore leaves the strict loop, and unordered values cannot remain
on the native backedge.

## Runtime and lifecycle proof

The native fixture records `(0.5, 20.25, 2.0) -> 32.0`, then reuses the same
root with the exact-binary live-argument tuple
`(0.625, 5.5, 3.0) -> 5.625`. Replacing only `x`, `limit`, or `factor` with
its recording value instead produces `13.5`, `50.625`, or `10.0`. All three
calls leave through final exit 4, so their distinct iteration counts and
results detect specialization of every parameter. Root count, prototype
ownership, link type, IR, snapshots, allocation and decoded machine code are
revalidated after every reuse.

Strict equality is covered at all three boundaries:

- `(0.5, 2.0, 2.0) -> 2.0` reaches equality at the body guard and completes
  through final exit 4;
- `(0.5, 1.0, 2.0) -> 1.0` reaches equality after the first MUL and leaves
  through precondition exit 2; and
- `(1.0, 1.0, 2.0) -> 1.0` fails the interpreter's initial strict comparison
  and never publishes a native entry or exit.

All five exits are exercised while sides remain closed:

- exit 0: accumulator or factor NUM type guard;
- exit 1: limit NUM type guard;
- exit 2: strict ascending preheader comparison;
- exit 3: XPOLL for profile or STOPREQ work; and
- exit 4: final body comparison.

An integer accumulator `(1, 20.25, 2.0) -> 32.0` demonstrates exit-0
interpreter conversion followed by native re-entry and exit 4. Integer factor
`(0.5, 20.25, 2) -> 32.0` repeatedly exercises exit 0, while integer limit
`(0.5, 20, 2.0) -> 32.0` repeatedly exercises exit 1.
`(15.0, 20.25, 2.0) -> 30.0` exercises precondition exit 2. Repeated hot exits
leave `trace_nchild == 0` and `trace_nextside == 0`.

### Genuine NUM input and the IR_CONV boundary

Lua source spelling alone does not prove a genuine NUM TValue in this
dual-number runtime. An integral literal such as `2.0` can be INT-tagged. If
that value is the factor during recording, numeric multiplication introduces
an INT-to-NUM `IR_CONV`; opcode 93 is then rejected by the exact constant-free
NUM profile and no trace is published. This is expected fail-closed behavior,
not a missing FMUL lowering: `IR_MUL` is opcode 45 and the ARM64 backend
already has `IR_MUL -> asm_mul() -> A64I_FMULd`.

The C runtime certificate passes every positive record and reuse argument
through `lua_pushnumber`, including factor `2.0`, so the admitted path is
genuinely NUM. Its integer-factor case deliberately uses `lua_pushinteger`
and proves the separate type exit. A live Lua probe can likewise use a
runtime-produced NUM such as `math.sqrt(4)`; that exact factor publishes the
MUL loop root, while a direct INT-tagged `2.0` probe remains interpreted.

Profile and STOPREQ requests are published after admission and before native
SLOAD/FMUL execution. Profile work reaches XPOLL exit 3, cleans up, re-enters
the same root and finishes at exit 4. STOPREQ reaches exit 3 with the expected
VM-shutdown error, advances and acknowledges the epoch, releases the handshake
leader, drains pending/request/poll/profile state, consumes STOPREQ freshness,
restores idle VM/native-depth state and the pre-call C frame, and leaves the
root reusable after the sticky stop bit is explicitly cleared.

The fixture also replaces every live argument after admission and before its
native load or first FMUL, using the finite baseline
`(0.5, 20.25, 2.0) -> 32.0`:

- `x = qNaN` exits at 2 and returns NaN;
- `x = +Inf` exits at 2 and returns positive infinity;
- `x = -Inf` would remain negative infinity under multiplication by two, so
  simultaneous STOPREQ proves safe interruption through XPOLL exit 3;
- `limit = qNaN` and `limit = -Inf` exit at 2 and return the first product,
  `1.0`;
- `limit = +Inf` keeps the ordered finite backedge until multiplication
  overflows, then returns positive infinity through final exit 4;
- `factor = qNaN` exits at 2 and returns NaN;
- `factor = +Inf` exits at 2 and returns positive infinity;
- `factor = -Inf` makes the first product negative infinity and the next
  product positive infinity, then returns through final exit 4;
- `factor = 0.0` leaves the accumulator below the limit indefinitely, so
  simultaneous STOPREQ proves XPOLL exit 3; and
- `factor = 1.0` likewise leaves the accumulator unchanged, so simultaneous
  STOPREQ proves XPOLL exit 3.

The finite zero and one mutations use distinct post-admission request types,
not NaN/Inf aliases. Each exceptional or nonterminating mutation is followed
by complete handshake cleanup, exact-root revalidation, C-frame restoration
and finite exit-4 reuse. Admission adds no sign, progress or finiteness guard;
the certified `XPOLL` keeps STOPREQ available for the selected intrinsically
nonterminating cases. Trace flush restores the original `BC_LOOP` and removes
the runnable root.

## Negative closure

The synthetic fixture exercises the complete
`7 profiles x 2 pre-arithmetic ops x 2 body-arithmetic ops x 4 pre-guards x
4 body-guards` product: 448 combinations at the semantic gate and the same
448 at post-RA. Exactly the seven coherent named rows are admitted at each
gate. Coherence is derived from the active profile fields rather than
profile-index arithmetic. Each profile is crossed with its exact recurrence
and one distinct adjacent recurrence; the exhaustive per-profile mutation
suites close every remaining arithmetic family independently.

The exhaustive semantic, prototype, bytecode, snapshot and post-RA mutation
suites run for all seven profiles. They reject every changed IR field,
prototype identity field, bytecode opcode/operand/jump field, trace bound,
snapshot header/restored value/footer PC, spill, register-class violation,
impossible alias, split PHI register, nonzero stack adjustment, rename,
malformed terminal NOP, and extra or missing instruction. MUL-specific
mutations reject ADD, SUB, DIV, CONV, reversed MUL operands, mixed pre/body
arithmetic, integer MUL and MUL outside the exact `MUL_LT` prototype. The
source contract binds every profile selection to both exhaustive suites
inside `main()`, so one row cannot silently run twice while another proof
becomes dead.

Native negatives keep fixed-initializer and fixed-half variants interpreted.
Descending strict/inclusive MUL, descending MUL in the ADD profile families,
DIV variants, reversed comparisons and arithmetic operands, extra arithmetic,
wrong strictness and the existing SUB/ADD adjacent roots all publish no trace.
The obsolete exact `MUL_LT` no-trace negative was removed, and the runtime
contract rejects its accidental reintroduction. Exact `main()` invocations
and every remaining no-trace identity are pinned.

## Commits and validation

The tranche was published incrementally through `864b6aa0`:

- `c872eff5` - admit the exact semantic and post-RA `MUL_LT` profile with
  separately bounded MUL authorization;
- `7129ffc5` - add the seven-profile synthetic mutation, allocation and
  448-combination certificate;
- `0b789940` - add the ARM64/arm64e native runtime, lifecycle,
  exceptional-value and nontermination proof; and
- `864b6aa0` - name exact `MUL_LT/FMUL` in the umbrella gate summary.

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
- restoration of the exact ordinary ARM64 experimental helper with
  `src/lj_asm.o` current and byte-identical to its archive member,
  `LJ_TARGET_ARM64 == 1`, `LJ_ABI_PAUTH == 0`,
  `LJ_ABI_BRANCH_TRACK == 0`, the root-recorder and LOOP-native-entry gates
  open, and the broad side-recorder gate closed while the separately certified
  exact first-side production canary remains open;
- ten fresh-process live checks of the mixed-NUM, fixed-half, dynamic-step,
  `ADD_LT`, `ADD_LE`, `ADD_GT`, `ADD_GE`, `SUB_GT`, `SUB_GE`, and genuine-NUM
  `MUL_LT` roots, each publishing exactly trace 1 with
  `linktype == "loop"`; and
- at exact commit `864b6aa0`, the thin macOS x86_64 platform build and smoke,
  a real Rosetta JIT loop with
  `jit.os == "OSX"`, `jit.arch == "x64"`, trace 1 and
  `linktype == "loop"`, followed by the x86_64 stock suite: 509 passed.

The x86_64 platform builder's discarded preliminary non-GC64 diagnostic and
unused `topofs` warning are expected and pre-existing; the actual target
build, smoke and 509-test suite passed. The recurring ARM64 unused
`ccall_rawchild_wait` warning is also pre-existing.

## Boundary and next bounded tranche

This admission proves only the exact all-parameter strict ascending MUL
profile described above. It does not authorize `MUL_LE`, fixed-initializer
MUL loops, reversed operands, arbitrary ADD/SUB/MUL, division, conversions,
spills, calls, heap effects, side traces or stitches. The positive factor is
a property of the terminating certificate tuples, not an admission condition.
The NaN/Inf and zero/one proofs mutate arguments after admission; they do not
add or imply an admission-time finiteness or progress check. STOPREQ proves
safe interruption of the selected otherwise nonterminating cases, not
spontaneous termination.

This tranche exercises the established lockless root-entry, publication,
polling and cleanup paths for this one exact shape. It does not by itself
complete or prove the full lockless ARM64 VM, FFI and JIT port; that port
remains incomplete.

At the fully validated `MUL_LT` boundary, the next exact bounded
multiplication candidate had been reconnoitered:

```lua
local function f(x, limit, factor)
  while x <= limit do
    x = x * factor
  end
  return x
end
```

That strictness-adjacent `MUL_LE` follow-on began after this tranche, with a
later production checkpoint at `5af75596`. It is outside this note and does
not alter the `MUL_LT` implementation or validation boundary, which remains
exactly `864b6aa0`. Its own runtime, lifecycle, exceptional-value,
nontermination and negative-closure certification remains a separate tranche.

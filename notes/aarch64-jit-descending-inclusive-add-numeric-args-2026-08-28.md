# ARM64 JIT inclusive descending ADD all-parameter pure-NUM root

Date: 2026-08-28

## Scope

This tranche admits one additional exact macOS ARM64 root while keeping the
broad recorder, side-trace and stitch gates closed:

```lua
local function f(x, limit, step)
  while x >= limit do
    x = x + step
  end
  return x
end
```

The accumulator, limit and step are all live `NUM` parameters. This is the
inclusive descending `ADD_GE` counterpart to the already-certified strict
descending ADD root. The certified terminating tuples use a negative live
step, but admission deliberately adds no step-sign or finiteness guard. It
uses the same constant-free prototype, snapshot and allocation geometry as
the other strict/inclusive ascending and descending all-parameter roots.

The existing ARM64 backend already lowers NUM `IR_ADD` to double-precision
FADD. This work adds only an exact source-grammar admission and native-behavior
certificate. It adds no backend opcode, spill rule, conversion, call path,
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
3  ISGT  A=4 D=3
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
5. guarded `LIMIT <= X_PRE`;
6. `LOOP`;
7. `XPOLL 1`;
8. PHI-marked `X_BODY = X_PRE + STEP`;
9. guarded `X_BODY >= LIMIT`; and
10. `PHI(X_PRE, X_BODY)`.

The exact tuple is therefore
`BC_ISGT A=4 D=3 + IR_ADD(STEP,X) + IR_LE(LIMIT,X_PRE) +
IR_ADD(X_PRE,STEP) + IR_GE(X_BODY,LIMIT)`. It is not inferred from a
generic ADD or relational family.

The five snapshots are pinned to refs `X`, `LIMIT`, the preheader guard,
`LOOP`, and the body guard. Their map offsets are `{0,2,6,9,12}`, entry counts
are `{0,2,1,1,1}`, slot counts are `{5,6,5,5,5}`, `topslot` is 5, and footer
PCs are `{bc+6,bc+2,bc+11,bc+6,bc+11}`. There are exactly 15 snapshot-map
entries. The five restored entries are slots `{2,5,2,2,2}`, all with zero
flags, referencing `X_PRE` four times and `X_BODY` once. The base delta is
zero.

## Fail-closed production design

The all-parameter classifier now has six named full-shape profiles:

- `ADD_LT`: `ISGE A=3 D=4`, exact `ADDVV A=3 B=3 C=4`, pre/body `GT/LT`;
- `ADD_LE`: `ISGT A=3 D=4`, exact `ADDVV A=3 B=3 C=4`, pre/body `GE/LE`;
- `ADD_GT`: `ISGE A=4 D=3`, exact `ADDVV A=3 B=3 C=4`, pre/body `LT/GT`;
- `ADD_GE`: `ISGT A=4 D=3`, exact `ADDVV A=3 B=3 C=4`, pre/body `LE/GE`;
- `SUB_GT`: `ISGE A=4 D=3`, exact `SUBVV A=3 B=3 C=4`, pre/body `LT/GT`;
  and
- `SUB_GE`: `ISGT A=4 D=3`, exact `SUBVV A=3 B=3 C=4`, pre/body `LE/GE`.

`arm64_numacc_grammar_profile()` acquires the live comparison and recurrence
bytecodes and returns only one of those exact rows. The semantic and
independent post-RA kernels derive recurrence opcode, first-recurrence operand
order and both guards from the selected row. The helper that authorizes NUM
SUB still identifies only `SUB_GT` and `SUB_GE`; neither descending ADD
profile can expand that authorization. The fixed-initializer dynamic-step
root remains explicitly pinned to `ADD_LT`.

No generic arithmetic allowlist was widened. Every ADD producer must still
have NUM type, exact operands, exact PHI flags and the exact position in the
ten-reference shape. Post-RA repeats the restriction before checking layout.

The source contract scopes the complete `ADD_GE` tuple separately inside both
kernels through the following `SUB_GT` boundary. It also scopes the complete
four-profile ADD selector, including the exact `ADDVV A=3 B=3 C=4` recurrence
and each comparison A/D pair. It proves that `ADD_GE` is absent from the SUB
authorization helper. A follow-up contract hardening also moves the end of the
existing `ADD_GT` runtime-profile region to the new `ADD_GE` profile, so the
older profile cannot be accidentally proved by tokens from its new neighbor.

The admission contract refuses to link a stale assembler: `src/lj_asm.o` must
be newer than `src/lj_asm.c`, and the archive member must be byte-identical to
that object. Its arm64e audit explicitly enables BTI, verifies
`LJ_TARGET_ARM64`, `LJ_ABI_PAUTH` and `LJ_ABI_BRANCH_TRACK`, then compiles the
current assembler with `-Werror`.

## Post-RA and native certificate

The post-RA certificate requires a zeroed terminal `NOP`, zero stack
adjustment, no spills or renames, and FPR-only numeric values. The
loop-carried `X_PRE/X_BODY/PHI` family shares one register. `STEP`, `LIMIT`
and that family are pairwise distinct, and the original `X` cannot alias
`STEP`. Both first and body ADD operand sets are rechecked after allocation.

The observed allocation is `X=d2`, `STEP=d1`,
`X_PRE/X_BODY/PHI=d15`, and `LIMIT=d0`. Ordinary ARM64 emits exactly 136
bytes with `mcloop = 76`. ARM64e/BTI emits exactly 140 bytes with
`mcloop = 80` and a leading `BTI J`. Both forms contain exactly two
double-precision FADDs, no FSUB, and two double-precision FCMPs.

The first recurrence is `FADD d15,d1,d2`; the loop body is
`FADD d15,d15,d1`. Both comparisons are `FCMP d0,d15`. The preheader branches
`HI` to exit 2, so a limit strictly above the accumulator or an unordered NaN
comparison leaves native execution, while equality remains admitted. The body
branches `LS` back to the loop and otherwise follows the unconditional exit-4
branch. Equality therefore takes one more inclusive iteration; unordered
values cannot remain on the native backedge.

## Runtime and lifecycle proof

The native fixture records `(20.5, 0.25, -0.5) -> 0.0`, then reuses the same
root with the exact-binary live-argument tuple
`(0.375, -0.625, -0.25) -> -0.875`. Replacing only `x`, `limit`, or `step`
with its recording value instead produces `-0.75`, `0.125`, or `-1.125`.
Those calls leave through exits 4, 2 and 4 respectively, so both results and
exit paths detect specialization of every parameter. Root count, prototype
ownership, link type, IR, snapshots, allocation and decoded machine code are
revalidated after every reuse.

Inclusive equality is covered at all three boundaries:

- `(1.0, 0.25, -0.375) -> -0.125` reaches equality at the body guard, takes
  one more backedge and completes through final exit 4;
- `(1.0, 0.5, -0.5) -> 0.0` reaches equality after the first ADD, passes the
  inclusive precondition and completes through exit 4; and
- `(0.5, 0.5, -0.5) -> 0.0` enters from initial equality, then its first ADD
  falls below the limit and leaves through precondition exit 2.

All five exits are exercised while sides remain closed:

- exit 0: accumulator or step NUM type guard;
- exit 1: limit NUM type guard;
- exit 2: inclusive descending preheader comparison;
- exit 3: XPOLL for profile or STOPREQ work; and
- exit 4: final body comparison.

An integer accumulator `(20, 0.25, -0.5) -> 0.0` demonstrates exit-0
interpreter conversion followed by native re-entry and exit 4. Integer step
`(20.5, 0.25, -1) -> -0.5` and integer limit `(20.5, 1, -0.5) -> 0.5`
exercise their exact type exits. `(0.75, 0.5, -0.5) -> 0.25` exercises the
precondition exit. Repeated hot exits leave `trace_nchild == 0` and
`trace_nextside == 0`.

Profile and STOPREQ requests are published after admission and before native
SLOAD/FADD execution. Profile work reaches XPOLL exit 3, cleans up, re-enters
the same root and finishes at exit 4. STOPREQ reaches exit 3 with the expected
VM-shutdown error, advances and acknowledges the epoch, releases the handshake
leader, drains pending/request/poll/profile state, consumes STOPREQ freshness,
restores idle VM/native-depth state and the pre-call C frame, and leaves the
root reusable after the sticky stop bit is explicitly cleared.

The fixture also replaces every live argument after admission and before its
native load or first ADD, using the finite baseline
`(20.25, 0.25, -0.5) -> -0.25`:

- `x = qNaN` exits at 2 and returns NaN;
- `x = +Inf` remains positive infinity, so simultaneous STOPREQ proves safe
  interruption through XPOLL exit 3;
- `x = -Inf` exits at 2 and returns negative infinity;
- `limit = qNaN` and `limit = +Inf` exit at 2 and return `19.75`;
- `limit = -Inf` would keep the loop running, so simultaneous STOPREQ proves
  exit 3;
- `step = qNaN` exits at 2 and returns NaN;
- `step = +Inf` produces and retains positive infinity, so simultaneous
  STOPREQ proves exit 3; and
- `step = -Inf` produces negative infinity, exits at 2 and returns it.

Each mutation is followed by complete handshake cleanup, exact-root
revalidation, C-frame restoration and finite exit-4 reuse. The admission adds
no step-sign or finiteness guard: zero, positive and exceptional steps retain
the source program's possible nontermination, while the certified `XPOLL`
keeps STOPREQ available. Trace flush restores the original `BC_LOOP` and
removes the runnable root.

## Negative closure

The synthetic fixture exercises the complete
`6 profiles x 2 pre-arithmetic ops x 2 body-arithmetic ops x 4 pre-guards x
4 body-guards` product: 384 combinations at the semantic gate and the same
384 at post-RA. Exactly the six coherent named rows are admitted at each gate.
Coherence is derived from the active profile fields rather than profile-index
arithmetic. Mixed guard/arithmetic profiles, reversed IR operands, live
comparison-bytecode strictness/operand changes, live ADD/SUB recurrence
changes and every adjacent opcode remain closed independently.

The exhaustive semantic, prototype, bytecode, snapshot and post-RA mutation
suites run for all six profiles. They reject every changed IR field, prototype
identity field, bytecode opcode/operand/jump field, trace bound, snapshot
header/restored value/footer PC, spill, register-class violation, impossible
alias, split PHI register, nonzero stack adjustment, rename, malformed
terminal NOP, and extra or missing instruction. The source contract binds
each profile selection to both exhaustive suites inside `main()`, so one row
cannot silently run twice while another proof becomes dead.

Native negatives keep fixed-initializer and fixed-half inclusive-descending
ADD roots interpreted. Reversed `limit <= x`, reversed `step + x`, an
extra-ADD recurrence, and inclusive-descending ADD with MUL or DIV all publish
no trace. Existing ascending, strict descending ADD and descending SUB
negatives remain live. The obsolete exact `ADD_GE` no-trace negative was
removed, and the contract rejects its accidental reintroduction. Exact
`main()` invocations and every remaining no-trace identity are pinned.

## Commits and validation

The tranche was published incrementally through `86affd56`:

- `1e4ed1ae` - admit the exact semantic and post-RA `ADD_GE` profile;
- `ba346e75` - add the six-profile synthetic mutation, allocation and
  384-combination certificate;
- `80f03594` - add the ARM64/arm64e native runtime, lifecycle and
  exceptional-value proof;
- `542f0216` - name `ADD_GE` in the umbrella gate summary; and
- `86affd56` - harden the static scope of the neighboring `ADD_GT` runtime
  profile after inserting `ADD_GE`.

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
- nine fresh-process live checks of the mixed-NUM, fixed-half, dynamic-step,
  `ADD_LT`, `ADD_LE`, `ADD_GT`, `ADD_GE`, `SUB_GT`, and `SUB_GE` roots, each
  publishing exactly trace 1 with `linktype == "loop"`; and
- from a disposable checkout at exact commit `86affd56`, the thin macOS
  x86_64 platform build and smoke, a real Rosetta JIT loop with
  `jit.os == "OSX"`, `jit.arch == "x64"`, trace 1 and
  `linktype == "loop"`, followed by the x86_64 stock suite: 509 passed.

The x86_64 platform builder's discarded preliminary non-GC64 diagnostic and
unused `topofs` warning are expected and pre-existing; the actual target
build, smoke and 509-test suite passed. The recurring ARM64 unused
`ccall_rawchild_wait` warning is also pre-existing.

## Boundary and next bounded tranche

This admission proves only the exact all-parameter inclusive descending ADD
profile described above. It does not authorize fixed-initializer inclusive
loops, reversed operands, arbitrary ADD/SUB, multiplication, division,
conversions, spills, calls, heap effects, side traces or stitches. The
negative live step is a property of the terminating certificate tuples, not
an admission condition. The NaN/Inf proof mutates arguments after admission;
it does not add or imply an admission-time finiteness check. STOPREQ proves
safe interruption of the selected otherwise nonterminating cases, not
spontaneous termination.

This tranche exercises the established lockless root-entry, publication,
polling and cleanup paths for this one exact shape. It does not by itself
complete or prove the full lockless ARM64 VM, FFI and JIT port.

With all six currently named all-parameter comparison/recurrence profiles now
certified, a small next widening candidate is the still-interpreted
fixed-initializer inclusive ascending dynamic-step root:

```lua
local x = 0.5
while x <= limit do
  x = x + step
end
```

That candidate remains outside this admission and needs its own source
bytecode, semantic IR, snapshot, post-RA, machine-code, lifecycle and
exceptional-value certificate before it can be opened.

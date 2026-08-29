# ARM64 dynamic numeric-step root

Date: 2026-08-28

## Scope

This tranche admits one additional spill-free, pure-NUM `BC_LOOP` root on
macOS ARM64 and arm64e:

```lua
local function f(limit, step)
  local x = 0.5
  while x < limit do
    x = x + step
  end
  return x
end
```

Unlike the preceding fixed-half root, the increment is an invariant NUM
parameter. The trace therefore proves a second NUM SLOAD and uses it in both
ADDs. It does not admit a different initial constant, inclusive comparison,
subtraction, multiplication, division, conversion, call, heap effect, spill,
side trace or stitched trace.

## Exact certificate

The source prototype has five frame slots, fourteen bytecodes, two parameters,
no upvalues or GC constants, and one prototype KNUM whose bits are exactly
`0x3fe0000000000000` (`+0.5`). The trace starts at bytecode 6:

```text
0  FUNCF A=5
1  KNUM  A=2 K=0
2  CGET  A=3 D=2
3  CGET  A=4 D=0
4  ISGE  A=3 D=4
5  JMP   A=3 +6
6  LOOP  A=3 +5
7  CGET  A=3 D=2
8  CGET  A=4 D=1
9  ADDVV A=3 B=3 C=4
10 CSET  A=2 D=3
11 JMP   A=3 -10
12 CGET  A=3 D=2
13 RET1  A=3 D=2
```

The prototype constant initializes `x` before trace entry, so the compact
trace itself has no numeric constants: `T->nk == REF_TRUE`. Its ten semantic
instructions after `REF_BASE` are exactly:

```text
X          NUM|GUARD SLOAD #4 TYPECHECK
STEP       NUM|GUARD SLOAD #3 TYPECHECK
X_PRE      NUM|PHI  ADD STEP, X
LIMIT      NUM|GUARD SLOAD #2 TYPECHECK
PRE_GUARD  NUM|GUARD GT LIMIT, X_PRE
LOOP       NIL|GUARD LOOP
XPOLL      NIL|GUARD XPOLL 1
X_BODY     NUM|PHI  ADD X_PRE, STEP
BODY_GUARD NUM|GUARD LT X_BODY, LIMIT
X_PHI      NUM       PHI X_PRE, X_BODY
```

The five snapshots have refs `[X, LIMIT, PRE_GUARD, LOOP, BODY_GUARD]`, map
offsets `[0, 2, 6, 9, 12]`, entry counts `[0, 2, 1, 1, 1]`, slot counts
`[5, 6, 5, 5, 5]`, top slot 5, fifteen total map entries and prototype PC
positions `[7, 3, 12, 7, 12]`. They restore the current accumulator into slot
4, plus the extra pre-loop slot 5 in snapshot 1.

## Admission and allocation

Semantic admission pins all fourteen bytecodes, the prototype KNUM, all IR
tuples and operands, snapshots, footer PCs, root position and trace counts.
The no-constant IR interval reaches the existing all-KINT constant classifier
vacuously, but the new branch additionally requires `T->nk == REF_TRUE` before
entering the exact dynamic-step helper. Any KINT or KNUM contamination remains
closed. The fixed-half and mixed INT/NUM profiles retain their own independent
certificates.

Post-RA admission rechecks the complete compact shape under the root-entry
lease. The observed ARM64 and arm64e allocation is `X=d2`, `STEP=d1`,
`X_PRE=X_BODY=X_PHI=d15`, and `LIMIT=d0`, with no spills or RENAMEs, zero
stack adjustment and one terminal zeroed NOP. The proof accepts any allocatable
FPR assignment satisfying the actual liveness constraints:

- `X_PRE`, `X_BODY` and `X_PHI` use one register;
- `STEP` differs from that PHI family and from `LIMIT`;
- `LIMIT` differs from the PHI family; and
- `X` differs from `STEP`.

`X` may legally reuse the `LIMIT` or PHI register because its last use is the
first ADD. Synthetic tests positively exercise both legal aliases and reject
every prohibited alias, GPR assignment, FPR boundary violation and spill.

## Native behavior

The ARM64 body is 136 bytes with loop offset 76. The arm64e body is 140 bytes
with loop offset 80; its only additional word is the leading `BTI J` landing
pad. No assembler, exit-table, XPOLL or PAUTH feature was added.

Native decoding proves exactly two `FADD` and two `FCMP` instructions. The
preguard uses `B.HS` to exit 2. The loop-closing guard uses `B.LO` to the loop
body followed by the unconditional exit-4 branch. The dynamic step and limit
remain in distinct invariant FPRs while the accumulator remains in its PHI
register.

The runtime contract creates the root with step `0.5`, reuses it with step
`0.25`, and exercises all five exits: step type (0), limit type (1), preguard
(2), XPOLL (3), and final loop completion (4). Profile and STOPREQ requests
prove exit/re-entry, cleanup, handshake-leader release and later trace reuse.
Post-admission replacement of the live step with a quiet NaN or positive
infinity exits through snapshot 2 and returns NaN or positive infinity. Hot
type and precondition exits do not publish side traces.

Zero or negative dynamic steps retain the source program's possible
nontermination; the admitted trace preserves XPOLL and does not invent a sign
or finiteness guard.

## Regression repair

The first complete umbrella run correctly exposed one stale expectation in
the older fixed-half runtime fixture: it still classified this newly admitted
dynamic-step source as a negative case. That fixture now proves cross-profile
isolation instead: the same source publishes a five-slot trace with
`nk == REF_TRUE`, not the fixed-half KNUM profile. Its focused ARM64 and
arm64e/BTI contract passed before the umbrella was restarted.

## Commits and validation

The tranche was published incrementally:

- `6cee8bb7` - admit the exact semantic and post-RA dynamic-step root;
- `79b6b9da` - add the synthetic mutation and allocation matrix;
- `3819659e` - add the ARM64/arm64e native runtime and lifecycle proof; and
- `ba0775c5` - update the fixed-half fixture to prove profile isolation.

Validation completed on this Apple Silicon host:

- focused semantic/post-RA admission and runtime contracts;
- direct and randomized ARM64 plus arm64e/BTI execution;
- the complete `tools/ci/arm64_jit_fail_closed_gate.sh` umbrella after a clean
  restart, including all prior publication, side, exit, retirement, GDBJIT,
  callback, flush/reuse and safepoint contracts;
- the native ARM64 JIT-enabled vendored suite: 509 passed;
- the thin macOS x86_64 platform smoke, a real Rosetta loop with
  `jit.os == "OSX"`, `jit.arch == "x64"` and `linktype == "loop"`, and the
  x86_64 vendored suite: 509 passed; and
- restoration of the thin ARM64 experimental helper build, followed by real
  publication of fixed-half, dynamic-step and mixed-NUM roots.

The recurring compiler diagnostics were the pre-existing ARM64 unused
`ccall_rawchild_wait` warning and x86_64 unused `topofs` warning. The x86_64
platform builder's discarded preliminary non-GC64 clean diagnostic is also
pre-existing and the actual target build and smoke passed.

## Next bounded tranche

The smallest next widening is the same recurrence with all three values as
parameters:

```lua
local function f(x, limit, step)
  while x < limit do x = x + step end
  return x
end
```

Live ARM64 and arm64e reconnaissance found the same ten semantic IR
instructions, allocation and machine-code body, but a thirteen-bytecode,
three-parameter, constant-free prototype starting at bytecode 5. It needs a
second exact prototype/snapshot certificate, not a new backend feature.

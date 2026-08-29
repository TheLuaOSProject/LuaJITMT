# ARM64 pure numeric half-step root

Date: 2026-08-28

## Scope

This tranche admits one exact, spill-free, pure-NUM `BC_LOOP` root on macOS
ARM64 and arm64e:

```lua
local function f(limit)
  local x = 0.5
  while x < limit do
    x = x + 0.5
  end
  return x
end
```

It is the first admitted root whose loop-carried state, arithmetic and ordered
guards are all floating point. It deliberately does not admit other constants,
dynamic steps, conversions, subtraction, multiplication, division, calls,
heap effects, spills, side traces or stitched traces.

## Observed certificate

The prototype has four frame slots, thirteen bytecodes, one parameter, no
upvalues or GC constants, and one numeric constant whose bits are exactly
`0x3fe0000000000000` (`+0.5`). The root starts at bytecode 6. Its complete
bytecode grammar is:

```text
0  FUNCF A=4
1  KNUM  A=1 K=0
2  CGET  A=2 D=1
3  CGET  A=3 D=0
4  ISGE  A=2 D=3
5  JMP   A=2 +5
6  LOOP  A=2 +4
7  CGET  A=2 D=1
8  ADDVN A=2 B=2 C=0
9  CSET  A=1 D=2
10 JMP   A=2 -9
11 CGET  A=2 D=1
12 RET1  A=2 D=2
```

The two-cell KNUM occupies `REF_TRUE-2` plus its payload at `REF_TRUE-1`.
The semantic IR is exactly two NUM SLOADs, two NUM ADDs, ordered GT/LT guards,
one LOOP, one XPOLL and one NUM PHI. The post-RA trace has a single terminal
NOP, no RENAMEs, no spills, zero stack adjustment, FPR-only numeric values and
one common register for the pre-body value, body value and PHI.

The five snapshots have refs `[x, limit, preguard, loop, bodyguard]`, map
offsets `[0, 2, 6, 9, 12]`, entry counts `[0, 2, 1, 1, 1]`, slot counts
`[4, 5, 4, 4, 4]`, top slot 4, fifteen total map entries, and prototype PC
positions `[7, 3, 11, 7, 11]`. Snapshot values restore the loop-carried NUM
from its live FPR.

## Admission and isolation

`src/lj_asm.c` now distinguishes two constant profiles:

- the existing all-KINT profile for integer and mixed INT/NUM roots; and
- the exact two-cell `+0.5` KNUM profile for this pure-NUM root.

Semantic admission independently pins the prototype, all thirteen bytecodes,
KNUM header/payload, every IR instruction and operand, snapshots and footer
PCs. Post-RA admission reacquires the compact trace and independently pins the
same KNUM/IR/snapshot certificate, FPR classes, PHI register equality, NOP-only
suffix and zero-spill layout. An otherwise integer or mixed root cannot carry
the half-KNUM profile as an unused constant.

Native entry keeps the existing shared loop-admission marker, but revalidates
the trace-owned original start instruction, exact live JLOOP generation,
compact KNUM/IR/snapshots, topology and executable layout twice under the
`jit_base` lifetime lease. Rechecking all live prototype bytecodes at entry
would be wrong: another legitimate root in the same immutable prototype may
have patched a different bytecode to its JIT form. The full original grammar
is instead certified before publication.

First-side recording remains closed for this root. The only admitted parent
geometry is the integer canary with top slot 5 and nineteen bytecodes, while
this root has top slot 4 and thirteen bytecodes.

## Ordered floating-point proof

The ARM64 backend emits two `FCMP` guards. The invariant preguard branches
with `B.HS` to exit 2. The loop-closing guard uses its inverse `B.LO` backedge
and an unconditional branch to exit 4. ARM64 unordered comparison sets carry,
so a NaN takes the preguard exit and cannot take the loop backedge.

The native fixture proves this dynamically by pausing after root-entry
admission, release-storing a quiet NaN into the live limit slot, then allowing
native execution to continue. The trace exits through snapshot 2 and restores
`x == 1.0`; a later ordinary call reuses the same trace successfully.

## Proof coverage

The synthetic matrix mutates the KNUM header and payload, every semantic IR
family and operand, guard polarity and order, PHI marks, prototype geometry,
snapshot headers/maps/footers, FPR classes, spill state, stack adjustment and
post-RA suffix. It also proves that quarter-step, negative, infinity, NaN,
extra-constant, conversion, SUB, MUL, DIV, call and heap-effect variants remain
closed.

The native contract runs direct and randomized ARM64 plus arm64e/BTI builds.
It checks the published IR and snapshots, decodes the emitted FCMP/conditional
branches using the dynamically allocated FPRs, verifies the final result,
forces type and precondition exits, proves profile XPOLL exit/re-entry, proves
STOPREQ cleanup and trace reuse, checks handshake-leader release, exercises the
quiet-NaN exit, and confirms that hot side exits do not publish children.

## Commits and validation

The tranche was published incrementally:

- `804e3895` - admit the exact semantic and post-RA half-step root;
- `b83f057b` - separate the existing mixed-NUM runtime policy;
- `086ea7f4` - isolate constant profiles and reject unused contamination;
- `a05c24e9` - add the synthetic semantic/post-RA mutation matrix; and
- `ca9a2d21` - add the ARM64/arm64e native runtime and lifecycle proof.

Validation completed on this Apple Silicon host:

- the focused synthetic admission contract;
- the focused pure-NUM runtime contract on ARM64 and arm64e/BTI, direct and
  randomized;
- the complete `tools/ci/arm64_jit_fail_closed_gate.sh` umbrella, including
  root/side publication, GDBJIT, callbacks, FFI result lifetime, LOOP/FORL/
  JFUNCF entry, exits, retirement, flush/reuse and safepoints;
- the native ARM64 JIT-enabled vendored suite: 509 passed;
- the thin macOS x86_64 platform smoke, a real published Rosetta JIT loop, and
  the x86_64 vendored suite: 509 passed; and
- restoration of the thin ARM64 experimental helper build, followed by real
  native publication of both the mixed-NUM and pure-NUM roots.

The only compiler diagnostics were the existing ARM64 unused
`ccall_rawchild_wait` warning and the existing x86_64 unused `topofs` warning.
The x86 builder's discarded preliminary non-GC64 clean diagnostic is expected.

## Next bounded tranche

Keep the exact three-stage certificate and widen one dimension at a time. A
useful next candidate is a pure-NUM loop with a dynamic NUM step: it reuses the
now-proved ordered-guard and FPR paths while adding one invariant SLOAD and a
new exact snapshot/prototype shape, without introducing conversion, spill,
call, heap or side-trace behavior.

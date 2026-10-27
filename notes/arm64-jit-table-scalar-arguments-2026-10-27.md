# ARM64 scalar table arguments to CALLXS

This increment admits guarded INT, NUM, TRUE and FALSE results from bounded
rooted table reads inside existing fixed-signature scalar FORL CALLXS roots.
The results can supply the existing integer, double and float ABI carriers.
The bounded scalar helper retains its primitive-key restriction; the generic
rooted hit helper can also return a scalar through the existing GC-key lease
protocol. Nested table receivers retain their prior independent proofs.

## Admission and scope

Both semantic and allocated passes independently certify the helper, scratch
roots, success guard, exact guarded VLOAD, source TGETS/TGETV/TGETB, retry PC,
source map and following result snapshot. For booleans, the guarded VLOAD
remains the type witness while the result map uses the recorder's canonical
REF_TRUE or REF_FALSE. A mismatched canonical primitive is rejected.

After a certified scalar read, subsequent CALLXS operations also need the
existing exact XSAVE argument-source proof. This binds a numeric conversion
or folded boolean KINT to the actual saved Lua argument, including an
ordinary restorable slot and complete frame span. The semantic pass and
allocated pass derive this separately. Development mutation tests caught this
missing requirement in the initial draft; merely admitting the scalar VLOAD
would not have activated that proof for an SLOAD callable.

The existing CType, register/stack ABI, carrier conversion, live GC root,
physical location, native-frame and continuation checks remain required.
No emitter, recorder, table helper, GC2 runtime, atomic authority, native frame
or x64 production code changes. This opens composition of existing operations;
it does not add a new number-to-integer conversion or adjacent ABI classes.

## Focused evidence

At baseline `47c6e98b`, seven ordinary probes interpret correctly but reject
assembly at CALLXS: integer, double, float, true, false, a table-identity key
and a nested receiver. The initial draft publishes all seven with unchanged
results. Logs: `/tmp/ljmt-table-scalar-red.log`,
`/tmp/ljmt-table-scalar-red-ir.log` and
`/tmp/ljmt-table-scalar-green-ir.log`. Frozen validation below must be used for
the final source rather than these development binaries.

The native runtime fixture covers twelve executions with an active peer:

- TGETS integer, TGETV double, TGETB float, true and false converted to C int,
  plus a table-identity key returning a double.
- A second table read and native call after consuming the first call's result.
- Same-type peer replacement reusing the exact compiled trace.
- Nine integer arguments, including Darwin's stack argument; eleven mixed
  arguments with nine GPR and two FPR carriers.
- An INT-to-TRUE peer change exits at the original TGETS before any native
  finish, then performs exactly two interpreter calls.
- A forced POSTCALL exit resumes after CALL and preserves exactly one foreign
  effect per iteration.

It observes actual native-frame finishes, callable and trace identity, owner,
pins, synchronized active frame, errno, exact C counters and cleared state.
The helper library handle is retained by the configure closure.

The independent admission fixture starts with eight authentic traces. It
accepts each in both passes and rejects 156 semantic and 196 allocated
mutations. Cases include wrong helper/key domain, status guard, scratch
modes, CARG roots, VLOAD identity/tag/offset, source and result maps/PCs,
canonical booleans, exact XSAVE source/restore/span, conversion/ABI type,
register bank, scratch aliases and spill bounds. A half-slot spill is a
negative only for 64-bit NUM, since odd spill slots are valid for 32-bit INT.

All 16 fixtures in the extended table-call contract pass ARM64 assertions:
`/tmp/ljmt-table-scalar-focused.log`. The new tests are also registered in the
broader M7 scalar CALLXS suite. The focused run restores an ordinary ARM64
assertion runtime. The complete frozen-checkpoint results follow.

## Frozen checkpoint validation

Source checkpoint `df3269d19c9803e8b5ad690c347ebd7cd4b5c266` passes all 16
focused fixtures in all four profiles. Each profile includes eight authentic
scalar admission positives, 156 semantic and 196 allocated negative checks,
and actual native-frame runtime observations.

| Profile | Profile ID |
| --- | --- |
| ARM64 assertion | `42feea53d7fccbf3e09f2edd1c5a41b19eaa6a992e453f72467b54254c3f345c` |
| arm64e/BTI assertion | `e85a7bd977b16e5349c7c14fefbaedb7ab9c090646c4d8a58cf8db3ab33b7dfd` |
| ARM64 release | `40db248f962ebf98d0cb2bbca8f989f13912c83672fb54a8609c2825ab241ad3` |
| arm64e/BTI release | `2c22b3dcdbf1c551a45730065b60503b2ceb126b9dd89741167d45480e42af1c` |

Both complete umbrellas pass on the unchanged checkpoint:

- `tools/ci/arm64_jit_fail_closed_gate.sh`, including all four broader M7
  scalar CALLXS profiles with the new fixtures, shared-table and cell effects,
  GC lifetime, publication/entry/exit/flush, callback and unwind checks.
- `tools/ci/arm64_bootstrap_gate.sh`, including the assertion interpreter,
  stock tests, GC2 hard assist, TG hooks, coroutine/dead-resume and callbacks.

Logs: `/tmp/ljmt-table-scalar-four-profile.log`,
`/tmp/ljmt-table-scalar-experimental.log` and
`/tmp/ljmt-table-scalar-bootstrap.log`. This refreshes the complete umbrellas
that the preceding focused table increments had not rerun.

Ordinary ARM64 assertion profile
`3481901099bf483b5a6bbd002c6d3734d17b7d6a0e69739ad0279f5953f72e91`
passes eleven compiled scalar-table cases plus type-change retry, five nested
read cases, nine owned-I64 argument cases and nine table-callable cases. These
check exact results, C counters, errno, published JFORL traces and trace reuse
without test hooks. Bit operations and rooted cells/upvalues also pass.

Both stock source and original-bytecode import corpora pass 509 cases on this
ARM64 runtime. Signed x64/Rosetta assertion profile
`3657775ee9144fc2891591752ac9a922b72c2df7b447643a4dc9b204952d0609`
passes both 509-case corpora too. This is bounded compatibility evidence,
not a full x64 umbrella or native Intel performance measurement.

The matrix verifies unchanged source and restores ordinary ARM64 assertion
profile `dcb625c667675ff82e1842d4204c9e2c1944e36791996abb77f0c1d325127d91`.
The restored runtime repeats the new scalar smoke cases. Code-sign checks
pass; selected test-helper symbols and out-of-line atomic imports are absent.
Log: `/tmp/ljmt-table-scalar-matrix.log` and matching ordinary/stock/x64 logs.

The final ordinary probe `/tmp/ljmt-table-scalar-held-final.lua` separately
asserts correct results and absence of a published loop for the held-result
and fractional NUM-to-C-int cases; its accumulated-before-second-call control
must publish a loop. All assertions pass. The held-result assembly still
rejects with IR_RENAME, fractional narrowing with CALLXS. Log:
`/tmp/ljmt-table-scalar-held-final.log`. These gaps remain real after the final
source/XSAVE proof was added.

## Main checkout

Main `v2.1` was fast-forwarded to the source checkpoint and rebuilt with the
ordinary bootstrap/experimental/assertion flags, without test hooks. Profile
`8c9235e682fc3361f10742270070d34839eaaa99da339d1982c427d2fceae3ed`
passes the eleven scalar, five nested-read, nine I64-argument and nine
callable smoke cases, type-change retry/reuse, bit operations, rooted cells
and upvalues, and both 509-case stock corpora. Code-sign and atomic/test-helper
symbol checks pass. Log: `/tmp/ljmt-table-scalar-main.log`.

- Runtime SHA-256: `a3ee5cfb5b9de611c31b9286cdbedc00ef634e702704e21a03c7524afc099f07`
- Archive SHA-256: `67603e24556e79621e7f5a2b38c9690811a208589ec2da36b531564300501867`
- Build-signature SHA-256: `bb24a14de31e0aaf595d3e0109833478d0892d785669573471fa5ad42dfe1127`

## Remaining work

This is an incremental experimental JIT opening, not completion of the ARM64
port. Fractional NUM-to-C-int narrowing, general table PHIs, arbitrary Lua
calls/metamethod effects and other uncertified operations remain closed.

A concrete next gap is keeping a scalar CALLXS result live across a second
CALLXS before consuming it:

```lua
local a = fn(t.value)
local b = fn(t.value)
sum = sum + a
sum = sum + b
```

The ordinary probe `/tmp/ljmt-table-scalar-held.lua` returns 448 correctly but
rejects assembly with IR_RENAME; the accumulated-before-second-call variant
compiles. Log: `/tmp/ljmt-table-scalar-held.log`. IR_RENAME is also the generic
post-RA rejection marker, so it does not identify the failed predicate. The
first scalar call result is genuinely live across the second call in this
trace. Snapshot-specific allocation/rename handling is an audit candidate;
no fix or exact rejection diagnosis is claimed here. The runtime fixture's
read-after-call case does not cover that held-result shape.

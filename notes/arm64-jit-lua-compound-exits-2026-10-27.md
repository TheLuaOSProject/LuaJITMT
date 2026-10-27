# ARM64 compound Lua leaf child-frame exits

This checkpoint extends the integer child-exit checkpoint `539475c8`
(documentation `f666b05d`). The full ARM64 port remains incomplete.

## Scope and proof

The unary, straight-line, pure Lua leaf grammar is unchanged. Child-exit
admission no longer requires exactly one arithmetic operation and two child
registers. Each independent pass reconstructs the source values before the
snapshot's resume PC, resolves arithmetic through the surviving SSA values,
and binds every live child slot to its exact value. The child argument remains
in register zero and RET1 returns the last written register. Unsupported
source/IR transformations still refuse publication.

Integer constant folding, addition/subtraction by zero, multiplication by
zero/one/two, equal-operand subtraction and commutative reuse follow the
recorder's existing rules. An integer operation that can overflow must resolve
to its checked IR operation. Numeric conversion and arithmetic can form a
floating-point tail. Constant lookup follows `lj_ir_knumint` canonicalization
and preserves negative zero. There is no new optimizer, machine-code lowering,
stack-restore path or shared runtime mechanism, and no added lock.

The resume PC need not describe the instruction at `snap.ref`: CSE can remove
that source operation. In `(x+3)+(x+3)`, the snapshot resumes at the second
ADDVN while its first surviving guard is the final ADDVV. Conversely, both
operations in `(x+3)*2` share the first arithmetic snapshot. Source replay is
bounded by the current iteration; operations before the resume PC must resolve
to values already available before the snapshot ref. The source body is pure,
and the existing effect-continuation checks remain active for caller effects.
This is an exit-state certificate, not a complete independent recompilation
of every caller bytecode or unrestricted Lua call/return admission.

Closure identity, exact unary CALL, return-PC frame link, base delta and
maximum root/child frame extent retain their existing proofs. The dynamic
live child extent is checked separately from its full frame size. Missing,
extra, wrong-value and NORESTORE child entries are rejected. The allocated
pass independently reconstructs the source prefix and retains snapshot-specific
RENAME resolution and register/spill coexistence checks. Neither pass consumes
the other's acceptance result.

## Focused evidence

The C fixture has seven shapes: a shared-snapshot arithmetic chain, several
intermediate locals, repeated-expression CSE, a square, a NUM tail, a KSHORT
local and reversed subtraction. Against separately compiled, interpreted
oracles and with a live MT peer, it passes 48 native invocations, 17 forced
child exits, 31 same-trace reuse checks and seven reuses after GC. Checks bind
actual native entry and first-exit parent/snapshot, child PC, preloop/backedge
location, frame link and closure. Base/top offsets, cframe and native-phase
cleanup are checked after each invocation.

Private, unpublished clones pass 686 semantic and 714 allocated negative
checks, including eight physical register conflicts. Every mutation starts
from an accepted clone. The ordinary Lua fixture passes seven publications,
17 overflow ranges and 48 same-trace checks, with seven cold calls. Ordinary
publication is not a separate native-execution claim.

The preceding single-operation and floating leaf contracts also pass. The
old compound refusal has been replaced with a child-local assignment outside
the pure leaf grammar. The combined contract registers all three fixtures in
ARM64/arm64e assertion/release profiles.

Fixture development corrected mistaken source PCs, single-iteration ranges
that could not enter a FORL trace, copied-versus-live prototype addressing,
and a meaningless spill mutation on a constant. Those were test defects;
reported counts above are from the passing corrected fixture. A separate
12-form publication probe also passed, but does not broaden the native claim.

Six counterfactuals reproduced the intended failures. The preceding header
cannot publish a compound candidate. Removing exact saved-value binding or
complete-map coverage independently defeats the semantic and allocated
negative fixtures. Removing allocated coexistence admits a physical alias.
Production was restored byte-for-byte and all three focused C/Lua contracts
passed again.

Focused logs: `/tmp/ljmt-compound-only.log`,
`/tmp/ljmt-compound-counterfactual.log` and
`/tmp/ljmt-compound-restored-focused.log`. The source checkpoint and complete
matrix results will be recorded after frozen validation; the preceding
checkpoint remains the latest fully validated main checkout until then.

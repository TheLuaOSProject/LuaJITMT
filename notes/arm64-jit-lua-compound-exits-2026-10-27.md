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
`/tmp/ljmt-compound-restored-focused.log`.

## Frozen validation

Source checkpoint `6f38a9c38d1f747bb379ac567011f8ba4ab280b0` passes the
complete experimental and bootstrap gates. All 26 preceding table/FFI
fixtures pass in four profiles, as does the complete M7 FFI suite. The three
Lua leaf contracts pass in ARM64 and arm64e/BTI, with assertions and in release.
Every profile records the same 48 native invocations, 17 child exits, 686
semantic negatives, 714 allocated negatives and eight physical conflicts for
the compound fixture, with zero cold retries. The combined leaf profile
identities are:

| Profile | Identity |
| --- | --- |
| ARM64 assertions | `4b4b955cbcfaa298ac8a4066f0eb33deaedc74746bafd938061d9ba69af026ed` |
| arm64e/BTI assertions | `ef8576149418594269d1d03025ff75354ab1aa34bb545b110c43e591364ef217` |
| ARM64 release | `77882b6d8eca879dced4be0d1f3d0d1a8b6e483fe11ee3dd02510a19f31c9923` |
| arm64e/BTI release | `5e556864b0fd6fdb0689c43e4a4532daaabd7237dd36457d83a5b2547e1e7f99` |

Ordinary ARM64 and signed x64/Rosetta each pass both 509-case stock source and
original-bytecode import corpora. The ordinary ARM64 build also passes all
698 preceding observations, the numeric boundaries, bitops, cells, upvalues,
and all three leaf contracts. The worktree was restored to ordinary ARM64
assertion mode after the matrix, with frozen source identity verified.

The main `v2.1` checkout was fast-forwarded to the source checkpoint and
rebuilt independently in ordinary ARM64 assertion mode without test helpers.
It passes both stock corpora and the same ordinary regressions. Numeric CALLM
required zero cold retries; the 240 INT observations used 240 candidates.
All seven compound cases published on their first cold call, with 17 overflow
ranges and 48 same-trace checks. Those ordinary publication/reuse observations
do not replace the focused native-entry and exit evidence above.

Both ordinary executables pass codesign verification and contain neither
exported test helpers nor out-of-line atomic imports. The main profile is
`0fb9918edcd9f9fd3c3dc41732dff308d5b1564df7609fce3a5e8d52f8f983c1`.
Main and worktree executable SHA-256 values match:
`86d2181850e1065560f1364b445238c5859561d03277552374c0cf002f1a1152`.
Archive and build-signature values differ across checkout paths; archive
identity is not claimed.

Complete logs: `/tmp/ljmt-compound-matrix.log`,
`/tmp/ljmt-compound-four-profile.log`, `/tmp/ljmt-compound-experimental.log`,
`/tmp/ljmt-compound-bootstrap.log` and `/tmp/ljmt-compound-main.log`.

## Next bounded gap

Assignment to an already-defined numeric child local still emits CSET and
remains excluded by the pure leaf grammar. A bounded extension must prove
both the old destination and the source are numeric before treating CSET as
a local copy: the interpreter examines the destination's old tag and can
otherwise store through an existing closed cell. Function entry does not
initialize every frame slot, so absence of CHILD/FNEW alone cannot establish
that property for an undefined destination. Register zero must also remain
the argument anchor until an independent replacement provenance proof exists.
Any extension must preserve the added snapshot boundary and continue to
exclude captured cells, closure construction and nested calls. This is an
audited next step, not implemented support. General Lua call/return and the
full ARM64 port remain incomplete.

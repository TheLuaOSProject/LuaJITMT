# ARM64 integer Lua leaf child-frame exits

This checkpoint follows the fully validated pure Lua leaf checkpoint `a202c982`
(documentation `6cb4c26f`). The full ARM64 port remains incomplete.

## Scope

A scalar FORL root may retain a snapshot inside one fixed unary pure Lua
callee. The exact child body is FUNCF, CGET/MOV of argument zero to register
one, one integer ADD/SUB/MUL with a numeric constant, and RET1 of register one.
Both VN and NV operand orders are covered. The snapshot resumes at that
arithmetic instruction, with an exact guarded INT operation and constant.
Compound child arithmetic, deeper calls, branches, upvalues, varargs and multret
remain outside this child-frame certificate. Existing floating leaf traces
that retain only caller snapshots keep their preceding capability.

Semantic and allocated admission independently reconstruct the child closure,
raw KNUM return link, caller BC_CALL, base delta, parameter/temp slots, child
resume PC and maximum root/child frame extent. Both copies of the original
argument must agree with the guard operand, and the guarded literal must agree
with the child bytecode. A unique earlier callee-identity snapshot must restore
the exact caller CALL. The general snapshot bounds, root values, NORESTORE rules
and location checks still apply. Allocated child exits additionally reject
distinct saved values sharing a register or overlapping spill bytes, after
snapshot-specific RENAME resolution.

Published root `T->topslot`, native spill reservation, root entry and native
exit machinery are unchanged. The existing snapshot restore path consumes the
independently proved child extent and nonzero base delta. No locks or shared
runtime mechanism changes are introduced.

## Focused evidence

The C fixture covers six arithmetic operand forms and an additional root with
two live numeric accumulators. With a live MT peer and independently compiled,
permanently interpreted oracles, it requires 34 native runs, ten child overflow
exits and 24 same-trace reuses, including seven after GC. Entry counters and actual
first-exit parent/snapshot identify execution; child snapshots are checked for
the exact PC, base, closure and return link. Addition and multiplication
exercise backedge exits, and addition also exercises preloop overflow. Stack
base/top offsets, cframe and native-phase cleanup are checked after every call.

Private copies of the root and child prototypes, closure, IR and snapshot maps
exercise 406 semantic and 435 allocated negative cases, including a
snapshot-specific register conflict between caller values. Each mutation starts
from an accepted baseline. The live owner remains resident and fake copies are
never published or executed.

The ordinary Lua fixture checks six publications, eight overflow ranges and
28 exact-trace reuses against separate interpreted oracles. All six publish on
the first cold call. Publication alone is not native-execution evidence.
The preceding floating leaf C and ordinary contracts also pass, with their
old integer refusal replaced by a compound-integer child refusal.

Six counterfactuals detect the intended failures: the preceding production
admission cannot publish the new native test; omitting child temp binding or
literal/source agreement independently defeats semantic and allocated negative
tests; omitting the allocated coexistence check admits the conflicting RENAME.
Production source was restored byte-for-byte, and complete focused contracts
passed again. Adding commuted multiplication initially exposed a test mutation
that merely changed MULNV to equivalent MULVN; the mutation now changes the
operation, and the expanded focused contract passes.

Focused logs: `/tmp/ljmt-child-focused-final.log`,
`/tmp/ljmt-child-counterfactual.log`, and
`/tmp/ljmt-child-counterfactual-restored.log`.

## Complete validation and integration

The source checkpoint is `539475c87ec87da91ce7680d3d2988ee691c63f6`.
Source stayed frozen through the complete matrix. All four ARM64/arm64e
assertion/release profiles pass the new 34 native runs, ten child exits,
406 semantic negatives, 435 allocated negatives and the ordinary child
contract. The preceding floating leaf native and ordinary contracts also
pass in each profile. The leaf/child contract profile identities, in ARM64
assertion, arm64e assertion, ARM64 release and arm64e release order, are:

```
38d850e3bd08c26ee92b7176d99783e86bb4100e0281b017d7a5c4da801ff199
29d15ec607517258a8a4aa83a052e5431867450f6e98bc345cbc82f7073ce064
0daa4968a2ba40f179a1bf450e20fc4fd42746588c45e4114223ec1b2823eab7
3c0b84e4a0b12300df40c80bc2554ba176e934a911486208456171a0e7a6ed85
```

All 26 preceding table/FFI fixtures pass all four profiles. Both complete
experimental/bootstrap gates pass, including the four full M7 scalar CALLXS
suites. ARM64 and signed x64 each pass both 509-case stock source and original
bytecode import corpora. The ordinary ARM64 runtime passes the 698 preceding
observations, boundary/bit-operation/cell/upvalue checks, the floating leaf
publication/refusal contract and the new child contract. The worktree is
restored to ordinary ARM64 assertion mode with frozen source identity verified.

The main `v2.1` checkout was fast-forwarded to the source checkpoint and rebuilt
independently in ordinary ARM64 assertion mode, without test helpers. It passes
the same ordinary contracts and both 509-case stock corpora. Numeric CALLM
needed one bounded cold retry; the 240 INT observations used 240 candidates,
and all six new child cases published on the first cold call. Ordinary
publication/reuse checks do not replace the focused native execution evidence.
Both ordinary runtimes pass codesign verification and contain neither exported
test helpers nor out-of-line atomic imports.

The main profile identity is
`05486ac11460d297e1f78910584e1b44b4e1f890e5e4bf1ce9f2c50ec18a241b`.
Main and worktree runtime binaries have identical SHA-256:
`7dce37e3bb8920c46394b064442010f1fc7672ed0018f76b084dcd39340fb4d8`.
Archive and profile signatures differ across checkout paths; archive identity
is not claimed.

Complete logs: `/tmp/ljmt-child-matrix.log`,
`/tmp/ljmt-child-four-profile.log`, `/tmp/ljmt-child-experimental.log`,
`/tmp/ljmt-child-bootstrap.log`, and `/tmp/ljmt-child-main.log`.
The next bounded gap is compound arithmetic inside the pure leaf. An inlined
child return does not force a caller snapshot: `lj_record_ret` adds its return
snapshot only for the separate `framedepth == 0` path. Later pure caller
arithmetic can therefore reuse an earlier child snapshot. Further admission
must preserve the child temporaries and prove any replay interval remains free
of effects; it must not assume a mandatory root continuation snapshot.
General Lua call/return and the full ARM64 port remain incomplete.

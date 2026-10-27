# ARM64 integer Lua leaf child-frame exits

This candidate follows the fully validated pure Lua leaf checkpoint `a202c982`
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
exits, 24 same-trace reuses and seven post-GC reuses. Entry counters and actual
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

The new fixtures are included in all four profiles of the existing Lua leaf
contract. Full frozen-source validation and main integration remain pending;
`6cb4c26f` remains the latest fully validated main checkpoint.

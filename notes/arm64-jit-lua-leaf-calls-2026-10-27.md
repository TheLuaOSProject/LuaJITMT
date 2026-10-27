# ARM64 fixed pure Lua leaf calls

This checkpoint follows the fully validated CData32 argument checkpoint
`dfd8db52` (documentation `9b45da6f`). The full port remains incomplete.

## Scope and implementation

A scalar FORL root can inline a direct fixed unary Lua function returning one
scalar, provided its optimized trace retains only caller-frame snapshots.
The leaf has no upvalues, collectable constants, calls, branches, boxing or
heap access. The bounded complete bytecode walk accepts CGET/MOV, numeric
constants, ADD/SUB/MUL/DIV and a final RET1; it checks source registers and
constant indices and rereads every word, including the canonical FUNCF header.
It does not borrow the root's bytecode-generation authority for the child.

The recorder proves one child frame, the exact root caller and BC_CALL,
argument/result counts, actual and recorded base geometry, the specialized
callee closure, and the return PC. Snapshot creation can materialize a frame
marker as a KNUM carrying the raw return PC; that exact representation is
checked, rather than requiring the original empty marker. No emitter or native
entry/exit protocol changes are needed.

The recorder preserves an exact caller CALL snapshot before the callee identity
guard. Semantic and allocated validators independently match its function and
numeric argument slots and recheck the callee's pure source. Existing root-only
PC, frame extent, snapshot-value and location checks are unchanged. The generic
scalar certificate proves supported operations, operands and safe restoration;
it does not independently recompile bytecode to establish full source/IR
arithmetic equivalence. The existing recorder remains responsible for that
translation and for mapping RET1 into BC_CALL's result slot.

Integer leaves whose overflow guards retain a child-frame snapshot remain
refused, as do varargs, multret, extra arguments, nested calls, child upvalues
and control flow. This is not general Lua call/return or multi-frame exit
support. x64 production behavior is unchanged.

## Focused evidence

`tests/t-arm64-jit-lua-leaf-calls.c` uses three distinct floating-point leaf
forms and separate permanently interpreted leaf/runner oracles. With an active
MT peer it requires 12 native runs: normal reuse, exact callee-identity guard
exit for an alternate closure, return to the original trace, and reuse after
full GC. Entry success counters plus actual exits identify execution of the
exact runner trace. All three shapes pass with zero cold retries.

The same fixture checks 72 semantic and 78 allocated negative cases using
private prototype, function, IR and snapshot copies while the live owner remains
resident. These cover wrong caller slots/arity/result count, CALLM, continuation
PC, snapshot reference, child PC/base/frame/extent, malformed guards, child
source/arity/effects/registers/constants/return, and invalid allocated function
register/spill locations. Each mutation starts from an accepted baseline.

`tests/t-arm64-jit-lua-leaf-calls.lua` checks nine ordinary publication cases
with one exact-trace reuse at a different trip count, and seven refusal shapes
with eight correctly interpreted calls each. Publication alone is not native
execution evidence. All nine positives publish on their first cold attempt.

Five counterfactuals pass: the preceding production source fails the new
publication contract, and removing caller binding or leaf-source validation in
either independent validator makes the corresponding malformed-copy test fail.
Candidate production source was restored byte-for-byte and the complete focused
fixture and Lua contract passed again.

Focused logs: `/tmp/ljmt-lua-leaf-focused-final.log`,
`/tmp/ljmt-lua-leaf-counterfactual.log`, and
`/tmp/ljmt-lua-leaf-counterfactual-restored.log`.

The four-profile contract is registered in the complete experimental gate.
Full frozen-source validation and main integration are pending for this new
checkpoint; the preceding CData32 checkpoint remains the latest fully validated
main build until that validation succeeds.

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

## Complete validation and integration

The source checkpoint is `a202c982fc210b3704cbc309a133b670cd4af54e`.
Source remained frozen throughout the complete matrix. The four-profile leaf
contract is registered in the experimental gate and passes on ARM64 and
arm64e/BTI, with assertion and release builds. Each profile passes the 12 native
runs, 72 semantic and 78 allocated negatives, and the ordinary nine-publication,
nine-reuse and seven-refusal-shape contract, with zero leaf cold retries.

Leaf profile identities, in ARM64 assertion, arm64e assertion, ARM64 release
and arm64e release order:

```
4d55498b0757d03227491c2a18fefe44cf8c6ebc7d31efaafca4ad390c0dbb4d
5750f6c27401d92cef966192fc4d196620df5717466c647105102d126db4f194
525ab1500179db018b7406b1e61213c208ac8d5536607b67f6360361e7cfe5e6
c8fd3d2e2e3b64be4cdc4c6fecfaf29c4dff206f85979b63ecaa64b54079f59b
```

All 26 preceding focused table/FFI fixtures pass the four profiles. The complete
experimental and bootstrap gates pass, including all four full M7 scalar
CALLXS profiles. Ordinary ARM64 without test helpers passes the preceding 698
observations, boundary, bit-operation, cell and upvalue checks, plus the new
leaf contract. ARM64 and signed x64 each pass both 509-case stock source and
original-bytecode import corpora. The worktree was restored to ordinary ARM64
assertion mode and its source identity and clean state were verified.

After fast-forwarding the main `v2.1` checkout to the source checkpoint, an
independent ordinary ARM64 rebuild passes the same 698 observations and leaf
contract, the boundary/cell/upvalue/bit-operation checks, and both 509-case
stock corpora. The numeric CALLM smoke needed one bounded cold retry, and the
INT smoke used 241 candidates for 240 observations; the leaf cases all publish
on the first attempt. These ordinary checks do not replace the focused native
execution witnesses. Both ordinary runtimes pass codesign verification and
contain neither test-helper exports nor out-of-line atomic imports.

The main profile identity is
`80e3fd834aedb76bb0b00f92aebac891a6642bb3345941c1a5a77fa2922f46d8`.
The main and worktree runtime binaries have the identical SHA-256:
`9788550d83ce5759bd320546dfc32f61b86b624a6c29d11e003e3239fdaba228`.
Archive and profile signatures differ across checkout paths; the runtime
comparison does not claim archive identity.

Complete logs: `/tmp/ljmt-leaf-matrix.log`,
`/tmp/ljmt-leaf-four-profile.log`, `/tmp/ljmt-leaf-experimental.log`,
`/tmp/ljmt-leaf-bootstrap.log`, and `/tmp/ljmt-leaf-main.log`.
The next bounded gap is a child-frame snapshot for integer arithmetic; general
Lua calls, deeper stacks, varargs, recursion and the full ARM64 port remain
incomplete.

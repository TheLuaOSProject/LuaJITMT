# ARM64 numeric Lua leaf local assignments

This extends the compound child-exit checkpoint `6f38a9c3` (documentation
`4ceed755`). The full ARM64 port remains incomplete.

## Scope and implementation

Pure unary Lua leaves may now reassign an already-defined numeric local with
CSET. The destination must be nonzero and both source and destination must
have preceding numeric definitions. Argument register zero remains unchanged
as the child source-prefix anchor. Calls, branches, captures, closure creation,
heap effects and other unsupported leaf operations remain excluded.

This is a source and admission extension. The existing recorder already
represents a raw numeric CSET as an SSA slot assignment and requests a snapshot
before the next bytecode. No recorder, optimizer, machine-code lowering,
interpreter, GC2 or locking behavior changes.

The prior destination definition is necessary: the interpreter examines its
old tag and can write through a closed cell. Function entry initializes
parameters, not every frame slot, so an undefined destination could still
contain a stale cell value. `lj_trace_arm64_lua_leaf` therefore rejects CSET
to an undefined destination or from an undefined source. The semantic and
allocated child-prefix passes independently require both old and new refs to
be numeric before replacing the local's value. Existing complete-map,
exact-value, frame-link, extent and physical-coexistence checks remain active.

The current arithmetic resume-PC restriction is unchanged. The exercised
CSET/CGET snapshots merge into the following arithmetic continuations, as
the recorder already permits. This does not claim support for arbitrary
continuation PCs, argument reassignment, multiple arguments or unrestricted
Lua call/return.

## Focused evidence

The existing compound C/Lua fixture now has twelve shapes. The seven preceding
shapes are retained; five additions cover one reassignment, repeated writes,
an older local kept live across another local's write, a floating-point tail,
and storing a NUM result before a later integer overflow. The new native
exit cases include both preloop and backedge exits after local writes.

The assertion build passes 86 native invocations, 31 forced child exits,
55 same-trace reuses including twelve after GC, and zero cold retries, against
separately compiled interpreted oracles with an active MT peer. It checks
native-entry and first-exit counters, exact parent/snapshot and child PC,
frame links, and base/top/cframe/native-phase cleanup. These totals include
the preceding 48 runs and seventeen exits: the additions contribute 38 runs,
fourteen exits and five post-GC reuses.

Private clones pass 1,374 semantic and 1,432 allocated negative checks,
including eighteen physical conflicts. Each pass exercises six CSET-specific
mutation classes with counts `20/20/8/8/8/2`: wrong source, argument-register
destination, undefined destination, undefined source, stale INT local value
and a wrong same-typed NUM local value. Every mutation starts from an accepted
clone, and each class must run in both passes with equal nonzero counts.

The ordinary Lua fixture passes twelve publications, 31 overflow ranges and
86 same-trace checks with twelve cold calls. This is publication/result/reuse
evidence, not a separate native-execution claim. Both earlier floating and
single-operation child contracts also pass. The ordinary refusal label now
identifies its existing rejected shape as argument reassignment.

Four counterfactuals fail as intended: the preceding source grammar rejects
the new candidates; explicitly disabling CSET in both prefix models prevents
publication; omitting the post-write destination's exact saved-value check
independently permits a stale local in the semantic and allocated fixtures.
All modified production and fixture files were restored byte-for-byte, and
all three focused C/Lua contracts passed again.

An initial experiment mixed the preceding prefix header with the newly
expanded source grammar. That violates the old prefix helper's input
precondition and hits a numeric-constant-index assertion, so it is not counted
as a graceful-refusal counterfactual. The explicit disabled-model variant
replaces that experiment. No such mixed configuration is retained.

Logs: `/tmp/ljmt-localwrites-focused.log`,
`/tmp/ljmt-localwrites-counterfactual.log` and
`/tmp/ljmt-localwrites-restored-focused.log`. The source checkpoint and full
matrix are pending; `4ceed755` remains the latest fully validated main checkout
until integration is recorded.

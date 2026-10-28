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

The implementation was committed and pushed as `f39cdfe5`. All four
ARM64/arm64e assertion/release leaf profiles passed the counts above, and all
26 preceding table/FFI fixtures passed their four profiles. The first full
experimental gate then timed out in the rooted insertion fixture, running
`number rawloop numberhash`. The sampled main thread was waiting in
`lj_safepoint_handshake` from GC2 sweep progress during a trace exit; its peer
was waiting in `safepoint_ack_tg` from a channel's native leave. The frozen
matrix therefore did not pass and was not integrated into main.

A subsequent debug-symbol build passed 1,100 repetitions of that insertion
case without reproducing the timeout. The captured stacks did not include
the pending/request state needed to identify its cause. The investigation
found a separately reproducible same-value consumed-poll missed wake; see
[the safepoint regression](safepoint-consumed-wake-2026-10-28.md). Passing
repetitions do not explain the original timeout.

The earlier focused logs were temporary `/tmp/ljmt-localwrites-*` files and
are no longer present after the environment restart. New regression evidence
is retained under `/Users/frityet/Scratch/ljmt-consumed-wake-20261028`.

## Complete replacement validation and integration

The frozen source checkpoint `dad7e318` includes the local-write extension
and the shared consumed-poll correction. Its replacement matrix passes both
complete experimental and bootstrap gates, all 26 table/FFI fixtures in four
profiles, and the combined M7 scalar FFI suite in four profiles. The complete
rooted-GC contract, including the previously timed-out insertion case, passes
on ARM64/arm64e assertion/release. Every new compound fixture profile again
reports 86 native runs, 31 child exits, 55 same-trace reuses, twelve post-GC
reuses, zero cold retries, 1,374 semantic negatives, 1,432 allocated negatives,
eighteen physical conflicts and `20/20/8/8/8/2` CSET negatives per pass.

| Leaf contract profile | Profile hash prefix |
| --- | --- |
| ARM64 assertion | `07f28cc9bdab` |
| arm64e assertion with BTI | `671475491c93` |
| ARM64 release | `602e37a3c2cf` |
| arm64e release with BTI | `ebaa2ef2abc7` |

ARM64 and signed x86_64 each pass the 509-case source corpus and the 509-case
original-bytecode import corpus. The ordinary ARM64 build also passes the
leaf, single-child, compound-child, bitops, local-cell and upvalue Lua
contracts. This does not imply that every stock case ran natively.

`v2.1` was fast-forwarded to `dad7e318` and independently rebuilt. It passes
both 509-case corpora and the same six ordinary Lua contracts. The compound
Lua result is twelve publications, 31 overflow ranges and 86 same-trace
checks; this ordinary result remains distinct from the native C fixture
evidence. Main and worktree executables have the same SHA-256:
`d8ed08ad42efe5b2eda9840652f2b16af9e0b17916b7a204ea9efa1930314517`.
The ordinary build passes code-signature verification, imports no compiler
atomic helper, and contains none of the checked test-helper symbols.

Current logs are `matrix.log`, `experimental.log`, `bootstrap.log`,
`table-ffi-four-profile.log`, the four `*-stock-*.log` corpus logs, and
`main-verify.log` under the evidence directory above. The complete matrix
verified that the source checkpoint remained unchanged before restoring the
ordinary ARM64 assertion build.

The next call-parity work must replace the immutable unary-argument anchor
with exact caller argument maps that survive LOOP substitution. Multiple
parameters, modified parameters, branches and nested frames are still
outside this checkpoint; passing the bounded leaves does not establish
x86_64 LuaJITMT parity.

# ARM64 fixed numeric Lua arguments and parameter assignments

This extends the numeric-local checkpoint `f39cdfe5` and consumed-poll fix
`dad7e318`, integrated at `8d4c6e4e`. The full ARM64 port remains incomplete.

## Source and snapshot changes

Pure Lua leaves in scalar FORL roots can now have zero or multiple fixed
numeric parameters, including assignments to parameter zero or later
parameters. The caller must supply exactly `numparams` arguments and request
one result. The existing bounded linear numeric grammar remains: CGET/MOV,
already-defined numeric CSET destinations, numeric literals, ADD/SUB/MUL/DIV,
and RET1. Calls, branches, captures, varargs and heap effects remain excluded.
Parameter writes must use the fork parser's canonical CSET instruction;
arithmetic and copies define temporaries before assignment.

The old child-prefix proof used the unchanged unary parameter as its anchor.
That cannot establish the incoming values of modified parameters or map
multiple loop-carried arguments. The recorder now retains the exact incoming
CALL map, and LOOP preserves and substitutes that map using its normal
`loop_subst_ref` path. A narrow ARM64 predicate identifies pure Lua calls for
retention; it grants no admission authority and leaves effectful C/FFI
continuation rules unchanged. There are no new snapshot flags, fields,
sidecars, runtime counters or locking paths.

The semantic and allocated validators independently select the nearest
earlier root-frame snapshot. It must be the exact CALL from the child's
return link and the same preloop/backedge partition. Each validator proves
the callee identity through its KGC or a dominating exact function guard,
checks every argument's slot, flags and INT/NUM reference, and seeds its own
source-prefix reconstruction from that immutable incoming vector. Current
child parameter values cannot replace the vector. Complete child maps,
numeric old/new CSET values, exact frame extent and footer, and independent
register/spill coexistence checks remain required.

Some caller assignments make the original FORL index SLOAD an input to an
additional genuine PHI. Five existing index checks now permit only the
`IRT_ISPHI` annotation in addition to their exact INT/guard type. Each pass
still independently verifies that PHI annotations correspond to actual PHI
operands, together with the exact index slot/load mode, induction arithmetic,
snapshot and allocated register constraints.

A leaf can compile its own function-entry trace between caller invocations.
Its live header then becomes JFUNCF. The source predicate accepts this header
only with the exact frame size, a nonzero trace operand and a canonical FUNCF
saved source word from `proto_jit_startins_acq`. It rechecks every live source
word and the saved header before returning. This does not use the entry
trace's certificate to authorize the inlined body. IFUNCF and explicit
PROTO_NOJIT remain excluded.

## Focused evidence

The new C fixture exercises eleven shapes: zero, two, three, four and eight
arguments; mixed INT/NUM inputs; reordered and constant inputs; an invariant
first argument; writes to parameter zero and a later parameter; updated
caller locals; and two calls to the same leaf within one loop body. The
eight-argument shape uses every parameter. The single-call overflow shapes
exercise both preloop and backedge child exits.

The restored production ARM64 assertion build passes 58 native invocations,
18 child overflow exits, 40 same-trace reuses including eleven after GC, and
zero cold retries. Each invocation compares with a separately compiled
interpreted oracle while an MT peer is active. Native evidence requires a
successful entry, an actual exit-handler call and a first exit belonging to
the expected parent trace. Overflow checks additionally match the exact
child resume PC, frame link, callee and loop partition. Stack base/top,
cframe, TG native state and the root's self-link are checked after return.

Private trace/prototype clones pass 750 semantic and 798 allocated negative
checks. The latter include 48 physical aliases that still pass semantic
validation. Twenty-three mutation classes cover CALL geometry, arity,
JFUNCF/header-shadow integrity, index annotations and PHIs, missing or
nonrestorable arguments, swapped values, stale same-typed values and values
from the wrong loop partition. Every class runs in both validators. Prototype
clones rebase their inline `jit_startins` shadow pointers before validation.

The ordinary Lua fixture passes eleven publications, eighteen overflow ranges
and 58 exact-trace checks with eleven cold calls. This is publication/result/
reuse evidence; its counters do not claim native execution. Existing leaf,
single-operation child and compound-child C/Lua contracts also pass. The
ordinary leaf test moves its existing numeric parameter-write shape from the
refusal set into the positive set.

## Counterfactuals

Four clean counterfactual builds fail the new fixture as intended:

| Removed guarantee | Observed failure |
| --- | --- |
| CALL-map retention | The first eight-argument root does not publish. |
| Semantic incoming-vector binding, replaced with current child parameters | Caller-swap mutation 18 at snapshot 9 is accepted by the semantic pass. |
| Allocated incoming-vector binding, replaced independently with current child parameters | The same mutation is accepted by the allocated pass. |
| JFUNCF canonical-shadow support | The first normal eight-argument invocation reports 31 entries but zero exits; the parent-native witness fails. |

The last case is a useful distinction: the entries belonged to the callee's
separate function trace, while the caller's leaf eligibility check refused
its patched header. Entry counts alone would have produced a false native
claim. All counterfactual source files were restored byte for byte before
the focused contracts were rerun.

Evidence is retained under
`/Users/frityet/Scratch/ljmt-lua-arguments-20261028`, including
`focused-restored.log`, `arguments-restored.log`, `counterfactuals.log`,
the four `cf-*.log` outputs and their build logs.

The implementation was committed and pushed as `43d0a9c9`. Its first complete
experimental run passed all four leaf profiles with the counts above, then
stopped at the rooted-length fixture's post-full-GC native-entry/exit
assertion. The earlier STOPREQ/native-resume assertion passed. The failed
run did not capture which native counter was zero or the concurrent GC gate
state, so its cause is not established. A standalone diagnostic copy passed
100 repetitions, followed by 500 unchanged-fixture runs paired with 500
diagnostic-copy runs. Those 1,100 successes do not explain the initial failure.

The rooted-length fixture now prints entry/exit/helper counters, GC phase and
entry gate, TG request state and function/loop bytecode only on failure. Its
native assertions remain strict; no retry or interpreter result is accepted
as native evidence. The diagnostic fixture compiled and passed against the
same assertion runtime. Original logs are `experimental-initial-failure.log`
and `matrix-initial-failure.log`; isolated runs are in `tablen-repeat.log`
and `tablen-paired-repeat.log`.

## Complete replacement validation and integration

The frozen checkpoint `338d0344` passes the complete experimental and bootstrap
gates, all 26 focused table/FFI fixtures in four profiles, and the combined M7
scalar FFI suite in four profiles. All four leaf profiles again report 58
native runs, eighteen child exits, forty same-trace reuses, eleven post-GC
reuses, zero cold retries, 750 semantic negatives, 798 allocated negatives
and 48 physical conflicts. The preceding leaf, single-child and compound
native fixtures retain their earlier counts. The rooted-length contract
passes on both ARM64 and arm64e with its strict assertions and new diagnostics.
The initial failed run remains unexplained.

| Leaf contract profile | Profile hash prefix |
| --- | --- |
| ARM64 assertion | `275b78c50f99` |
| arm64e assertion with BTI | `fb3823b72bac` |
| ARM64 release | `ebb2bffd8834` |
| arm64e release with BTI | `7d3a0e272bef` |

ARM64 and signed x86_64 each pass the 509-case source corpus and the 509-case
original-bytecode import corpus. The ordinary ARM64 runtime also passes
seven Lua contracts: leaf calls, single-operation child exits, compound
child exits, fixed arguments, bitops, local cells and upvalues. Signed x64
passes the same eleven-case fixed-argument Lua fixture, with eighteen overflow
ranges and 58 exact-trace checks. Stock and ordinary Lua results include
interpreter fallback and are separate from the native C-fixture evidence.

`v2.1` was fast-forwarded to the frozen checkpoint and independently rebuilt.
It passes both 509-case corpora and all seven ordinary Lua contracts. Main
and worktree executables have the same SHA-256:
`7a9abbfff0ba7380a3ef37abee732a15bbca2c8e7021cd62b48070b12a1e4e72`.
Both ordinary ARM64 assertion builds pass code-signature verification, import
no compiler atomic helper, and contain none of the checked test-helper
symbols. These executables were built at `338d0344`; the final documentation
commit changes no production source.

Logs are `matrix.log`, `experimental.log`, `bootstrap.log`,
`table-ffi-four-profile.log`, the four `*-stock-*.log` corpus logs,
`x64-lua-arguments.log`, and `main-verify.log` in the evidence directory above.
The matrix verified that its source checkpoint remained unchanged and
restored the ordinary ARM64 assertion runtime before integration.

## Remaining limits

Exact numeric fixed arity is a bounded extension. Every argument and every
live child slot must be present in its required map; snapshot construction
can elide otherwise valid unused parameters and make such a shape remain
interpreted at this checkpoint. The subsequent
[sparse-child proof](arm64-jit-lua-sparse-exits-2026-10-28.md) removes that
dead-slot restriction. Extra/missing arguments, nonnumeric arguments, arbitrary
continuation PCs, child branches, nested calls, tail calls, varargs and
multiple returns remain outside this proof. General trace topology,
iterators and broader FFI admission still prevent x86_64 LuaJITMT parity.

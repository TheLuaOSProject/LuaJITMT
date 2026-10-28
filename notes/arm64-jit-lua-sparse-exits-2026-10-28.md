# ARM64 sparse Lua child snapshots

This extends the fixed numeric argument checkpoint `93a3777c`. Full lockless
x86_64 LuaJITMT parity remains incomplete.

## Source liveness and retained caller maps

Straight-line numeric Lua leaves can now omit parameters and locals from a
child snapshot when the remaining source bytecode cannot read their old
values. The semantic pass works backwards from RET1, killing definitions and
adding every source read. The allocated pass independently walks forwards
from the resume PC and records reads before a new definition. Neither uses
the other pass's state or acceptance result. Arithmetic reads count even if
their results are dead, operands are processed before an aliased destination,
and CSET requires the old destination tag as well as its source value.

Every mapped child value still has to match the independently reconstructed
source prefix. Duplicate slots are rejected. Full nslots/topslot geometry,
callee identity, return link, exact fixed arity, the complete incoming caller
argument vector, numeric CSET provenance and physical coexistence at actual
child exits remain required. Snapshot construction and restoration are
unchanged; no new flags, runtime storage, locks or generated instructions are
introduced.

The unused-tail case also exposed an allocated-map distinction. LOOP can
eliminate the invariant callee identity guard, leaving its retained CALL map
immediately before a child map at the same IR reference. The assembler's
backwards snapshot selection can select only the last map at that reference.
The allocated pass skips physical locations for the preceding CALL map only
when the adjacent equal-reference child map independently passes its complete
source/caller proof. All caller entries still pass structure, flags and value
checks, and the child receives its normal physical-location validation.
Without this distinction, an unused last argument computed as `i+2` has no
register at the nonexecutable map and incorrectly prevents publication.

The existing argument and compound omission mutations previously removed
slots that can be source-dead. They now remove an operand read immediately by
the resume arithmetic instruction. Their negative counts are unchanged.

## Focused native and mutation evidence

Six new cases cover unused middle and trailing parameters, eight arguments
with six unused parameters, a dead reassigned local, a parameter read after
an earlier overflow and a CSET whose old destination tag remains required.
Each case uses a separately compiled interpreted oracle and an active MT
peer. The assertion build passes 36 native invocations, twelve child overflow
exits split between preloop and backedge copies, 24 same-trace reuses and six
post-GC reuses. The native witness requires successful entry, an exit-handler
call and the first exit belonging to the expected parent trace. Child exits
also match their source PC, frame, callee and loop partition. Stack, cframe
and TG native state are checked after return.

The private-clone tests observe sixteen naturally absent child slots and
accept forty explicit dead-slot removals across the two independent passes.
They reject 88 semantic and 97 allocated mutations, including six physical
aliases, three deleted locations at executable caller maps and 120 combined
frame/ref/footer boundary checks. The unused-tail backedge explicitly proves
the equal-reference retained CALL map contains a nonconstant argument with
no physical location while the executable child omits it. Removing that
argument from the caller map remains a rejection in both passes.

The ordinary Lua companion passes six publications, twelve overflow ranges
and 36 exact-trace checks. Those counts prove publication/result/reuse only;
the native execution claims above come from the C fixture. All preceding
leaf, single-child, compound and fixed-argument C/Lua fixtures pass again.

Five counterfactual builds fail the new fixture as intended:

| Disabled guarantee | Observed failure |
| --- | --- |
| Semantic required-slot coverage | A missing resume operand is accepted. |
| Allocated required-slot coverage | The independent allocated missing-operand check accepts it. |
| Semantic CSET old-tag liveness | Removing the old local at CSET is accepted. |
| Allocated CSET old-tag liveness | The independent allocated old-tag check accepts it. |
| Equal-reference CALL-map distinction | The unused-tail root fails to publish. |

The production header was restored byte for byte after all counterfactuals,
then rebuilt; all five C/Lua contracts passed. Evidence is retained in
`/Users/frityet/Scratch/ljmt-lua-call-generalization-20261028`, especially
`counterfactuals.log`, `cf-*.log`, `sparse-strengthened.log` and the source
copies used for each counterfactual.

## Frozen matrix and main integration

Source checkpoint `2d335d1d` passes the complete experimental and bootstrap
gates, all 26 focused table/FFI fixtures in four profiles, and the combined
M7 FFI suite in four profiles. Each leaf profile repeats the new 36 native
runs, twelve child exits, forty accepted dead-slot removals, 88 semantic and
97 allocated negatives, with the same preceding contract counts. The strict
rooted-length native-witness tests pass on ARM64 and arm64e; the earlier
post-full-GC failure remains unexplained and is not attributed to this fix.

| Leaf contract profile | Profile hash prefix |
| --- | --- |
| ARM64 assertion | `0531ef374d62` |
| arm64e assertion with BTI | `c981da4f5e87` |
| ARM64 release | `82f35f2e4144` |
| arm64e release with BTI | `c315e9c8d9a4` |

ARM64 and signed x86_64 each pass both 509-case source and original-bytecode
import corpora. The ordinary ARM64 build passes eight Lua contracts: leaf,
single-child, compound, fixed arguments, sparse children, bitops, local cells
and upvalues. Signed x64 also passes both the fixed-argument and sparse-child
Lua companions. Stock and ordinary Lua results permit interpreter fallback;
they are separate from the native fixture claims above.

`v2.1` was fast-forwarded to the frozen checkpoint and independently rebuilt.
Main passes both 509-case corpora and all eight ordinary Lua contracts. Main
and worktree ordinary ARM64 assertion executables have the same SHA-256:
`e0729ea452bd8e9be72c0389c3d3fa8d4318da44cd4c2748db7b9a098b0db54a`.
Both pass code-signature verification, import no compiler atomic helper and
contain none of the checked test-helper symbols. The final documentation
commit does not alter the source used for these builds.

The complete logs are `matrix.log`, `experimental.log`, `bootstrap.log`,
`table-ffi-four-profile.log`, `*-stock-*.log`, `x64-lua-sparse-exits.log`
and `main-verify.log` in the evidence directory above. The matrix verifies
its frozen source and restores the ordinary assertion runtime before exit.

## Remaining work

This removes a generic dead-slot restriction within the current linear leaf
proof. Exact numeric fixed arity is still required. Extra/missing arguments,
nonnumeric arguments, branches, nested calls, tail calls, varargs and multiple
returns need further source and frame proofs. General trace topology,
iterators and broader FFI admission also remain short of x86_64 parity.

The next argument-normalization step must distinguish the supplied CALL
vector from the callee formal vector. `rec_call_setup` and
`trace_arm64_lua_leaf_frame` currently require equal counts; stock
`rec_func_setup` already fills missing formals with nil and trims maxslot to
numparams. Each independent admission pass will need to retain every supplied
caller argument for replay while constructing its own normalized formal
vector. Ignored GC arguments still need the native root/lifetime proof;
being unused does not by itself authorize them. This is a source audit and
design direction, not implemented or native-verified normalization.

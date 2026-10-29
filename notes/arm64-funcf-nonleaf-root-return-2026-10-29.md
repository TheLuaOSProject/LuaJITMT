# ARM64 nonleaf FUNCF return root

The ARM64 fixed-function root recognizer now admits the bounded nonleaf case
already published by the x86-64 topology probe: a fixed-argument function
loads one captured Lua callee, makes one fixed one-argument/one-result call,
continues with the returned scalar, and returns one result. Branches, varargs,
multiple calls, heap effects, and open results remain outside this source
shape.

The semantic snapshot walk replays the root bytecode and the admitted leaf
body independently at each snapshot PC. It binds the call's callee and
argument to guarded IR, checks source-live locals against each root map, and
requires the exact returned value in the terminal `RET1` snapshot. The
post-RA walk repeats source and result reconstruction through its own
allocated-IR and snapshot view before publication. The root still uses the
existing rooted upvalue read and the ordinary scalar/leaf admission checks.

The first arm64e topology run exposed a lower-bound error in the rooted UGET
source scan. FUNCF has `J->loopref == 0`, but the old fallback selected ref 0
in the biased trace IR vector. LLDB confirmed `before=32769`, `end=T->nins=32781`,
and `J->loopref=0`; the scan then read outside valid IR references. The scan
now uses the loop reference only when it is in `[REF_FIRST, before)`, and
otherwise starts at `REF_FIRST`.

## Validation

`JOBS=2 tools/ci/arm64_jit_funcf_record_contract.sh` passed on arm64 and
arm64e assertion/release builds. The contract runs the topology probe in each
profile. The probe's `nonleaf-function-root-return` rows show a selected root
with a return trace (`nins=13`) and 81 selected-root native exits on the bad
argument phase, after collection with a live MT peer. Standalone output was
also recorded at `/tmp/funf-arm64-topology.tsv` and
`/tmp/funf-arm64e-fixed.tsv`.

This validates the one captured-call shape only. It does not establish tail
calls, recursion, multiple returns, open results, protected calls, or arbitrary
nonleaf effects.

## Follow-up scope repair

The first integration attempt let the new one-result inline-frame handling
fall through the generic FORL child-exit validators. A private `C_BASE_ZERO`
mutation then passed post-RA admission. The one-result continuation replay is
now restricted to FUNCF roots; generic roots retain their prior one-result
early-return rule, and inline child frames outside FUNCF still require the
previous two-result shape. The semantic root-snapshot replay also checks that
the next map offset does not precede the current offset before subtracting.

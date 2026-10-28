# ARM64 nested fixed Lua calls

This checkpoint admits a bounded nested fixed-call case in the experimental
ARM64 root-recorder path. A loop root may call a pure Lua function with one
result; that function may call another admitted pure Lua function with one
result, copy the returned value into its caller continuation, and return it.
The child function is passed as a parameter. Zero-argument fixed calls remain
valid (`BC_CALL C=1`); the argument count is always decoded as `C-1`, while
the one-result requirement is `B=2`.

The runtime preflight checks the live frame chain and exact source CALLs before
recording. It does not read a `TUPVAL` body through a raw pointer. The semantic
and post-RA admissions separately replay source arguments, function identity,
frame links, caller result binding, and snapshot locations. Frame function
slots remain exact KGC constants; dynamic function values are accepted only
through the separate exact identity-guard lookup used for the source CALL.
CALL liveness skips the reserved `LJ_FR2` slot between callee and first
argument, and derives the argument range from the encoded C field.

The fixture uses the repository's existing copied-trace proof pattern. It
mutates malformed frame function/link, argument, CALL arity, return pc,
continuation pcbase, and result-copy relationships and requires independent
semantic and post-RA rejection. A native run keeps a threading peer attached,
checks the root-owned exit and same-trace reuse, forces exact inner and outer
function identity exits, then runs again after full GC.

Validation on native Apple Silicon:

- `tools/ci/arm64_jit_lua_leaf_contract.sh`: all four profiles passed
  (`arm64-assert`, `arm64e-assert`, `arm64-release`, `arm64e-release`).
- `tests/t-arm64-jit-lua-nested-calls.c`: passed in each of those four
  profiles, including native entry, malformed semantic/post-RA fixtures,
  identity exits, live-peer execution, and post-GC reuse.

Scope remains bounded: this does not admit a captured child function through a
`UGET`/`TUPVAL` source, arbitrary variable-result or tail calls, arbitrary
continuation bytecode, or a two-child-frame snapshot chain. In the exercised
trace the inline snapshot has one child frame; the nested call and its return
are proven by recursive source replay and exact continuation binding.

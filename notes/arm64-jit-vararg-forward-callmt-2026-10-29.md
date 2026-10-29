# ARM64 fixed vararg tail forwarder

This checkpoint admits one bounded forwarding shape: a zero-formal vararg Lua
closure with one immutable upvalue, `VARG` in open-result mode, and a terminal
`CALLMT` to a fixed three-parameter numeric leaf. The target and its source
closure are bound independently in semantic and post-RA replay. The forwarding
frame proof checks the actual `FRAME_VARG` header, bytecode call window, and
snapshot copies for all three arguments and the exact callee. The child proof
then binds the current root `CALL` and the same argument window to the target.
Unsupported forwarding shapes continue to fail closed.

The C fixture exercises native entry, the paired CALL/FRAME_VARG child map,
argument-window mutations, an exact child overflow exit, mutable target
replacement, a live peer, collection, and re-entry. Its sampled boolean
primitive-result case passes the interpreter oracle but is still refused by
the caller-result grammar; it is not a native-support claim. The ordinary Lua
companion checks interpreter results and publication for the fixed and
forwarded cases.

## Validation

On 2026-10-29, `tools/ci/arm64_jit_lua_varargs_contract.sh` passed all four
profiles (`arm64-assert`, `arm64e-assert`, `arm64-release`, and
`arm64e-release`) with `JOBS=4`. Each profile reported 12 native entries, one
exact child overflow exit, and 24 semantic plus 24 post-RA negative checks.
The ordinary Lua companion also passed in each profile. Final full output is
at `/Users/frityet/Scratch/ljmt-varargs-forward-four-profile-final.log`.

The cleanup review found three diagnostic-removal hazards in post-RA proof
code. Two empty `if` statements had been left before real checks, making
source/argument rejection conditional on a chunk name and guard-snapshot
analysis conditional on the forwarder identity. A third chunk-size/name
condition had limited the missing-snapshot rejection to the expected fixture
proto. All three diagnostic conditions were removed; proof checks now run for
every trace. An earlier four-profile run before this last fix is superseded.
The final source passed `git diff --check`, a `-Werror` ARM64 build, and the
four-profile contract listed above.

The source-level paired frame formula is based on LuaJIT's architecture frame
headers, not a fixture-specific offset: x64's `NARGS` header, x86's distinct
slot convention, ARM64's `RC+16` header, and `rec_varg`'s `frame_delta` all
encode the forwarded fixed actual-argument extent plus `FRAME_VARG` and the
architecture's `LJ_FR2` slots.

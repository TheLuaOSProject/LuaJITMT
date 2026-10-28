# Captured callee upvalues in bounded ARM64 Lua leaves

The bounded ARM64 nested-call proof now covers a child Lua function that reads
its own closed function upvalue with `UGET` and calls that returned function
through an ordinary fixed-result `CALL` (`B == 2`). The runtime recorder uses
the rooted upvalue helper; generated code does not retain or dereference a raw
shared-cell pointer. The child frame's saved function is bound to its exact
function constant, and the UGET helper's source PC, UV index, rooted function,
pre-read snapshot and post-read continuation are checked independently by the
semantic and allocated-view passes. Existing call identity guards still own
function changes and exit through the exact native guard.

The C fixture exercises native entry and reuse for this captured nested case,
changes the captured callee to an alternate function to force the recorded
identity exit, restores it, runs collection, and reuses the published trace.
Private-copy mutations independently reject the semantic and allocated proof
when the UGET constant index, UGET bytecode index, or pre-read source PC is
changed. The ordinary parameter-passed nested-call fixture remains in the same
suite.

Scope remains bounded: the function is read through the closed-upvalue helper;
raw open cells, upvalue writes in nested frames, arbitrary closure effects,
variable-result calls and general multi-frame snapshot chains remain rejected.

Validation on 2026-10-28:

- `tools/ci/arm64_jit_lua_leaf_contract.sh`: passed the ARM64 and arm64e
  assertion and release profiles.
- `tests/t-arm64-jit-lua-nested-calls.c`: passed in all four profiles,
  including native entry, identity exit, post-GC reuse and independent
  malformed-source rejection.

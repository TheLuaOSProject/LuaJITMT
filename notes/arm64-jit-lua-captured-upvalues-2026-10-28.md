# Captured callee upvalues in bounded ARM64 Lua leaves

The bounded ARM64 nested-call proof now covers the nested-leaf workload shape:
the loop closure captures its leaf function, and that child reads its own
closed function upvalue with `UGET`, calls the returned function through an
ordinary fixed-result `CALL` (`B == 2`), then performs arithmetic on the
returned value. The root and child have one active inline-frame map at a time;
the source evaluator proves the child's further `CALL` and arithmetic, but the
fixture does not claim a two-child-frame snapshot chain. The runtime recorder uses
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
changed, when the rooted function source is below `nk` or a KGC payload, or
when a separately guarded dynamic function is substituted for this frame's
function. It also rejects an invalid source footer before dereferencing it.
The ordinary parameter-passed nested-call fixture remains in the same suite;
its exact dynamic frame-function operand is exercised as a positive source.

Scope remains bounded: the function is read through the closed-upvalue helper;
raw open cells, upvalue writes in nested frames, arbitrary closure effects,
variable-result calls and general multi-frame snapshot chains remain rejected.

Validation on 2026-10-28:

- `tools/ci/arm64_jit_lua_leaf_contract.sh`: passed the ARM64 and arm64e
  assertion and release profiles.
- `tests/t-arm64-jit-lua-nested-calls.c`: passed in all four profiles,
  including native entry, identity exit, post-GC reuse and independent
  malformed-source rejection.
- The fixture counted `SNAP_FRAME` entries in the actual loop traces:
  parameter-passed `nested_run` had one one-child-frame snapshot and no
  two-child-frame snapshots; captured `nested_capture_run` had six one-child
  snapshots and no two-child snapshots. The second source-level call is
  replayed from the child prototype and its result is bound through arithmetic.

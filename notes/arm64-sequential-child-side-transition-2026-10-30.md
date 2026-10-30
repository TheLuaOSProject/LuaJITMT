# ARM64 child-frame side transition audit (2026-10-30)

This is a diagnostic checkpoint for the isolated `codex/arm64-seq-child-side`
worktree, based on `d06f48e13`. It does not claim child-frame side publication
or change the production admission boundary.

## Observed transition

The side-exit snapshot is restored before `trace_hotside()` runs. At both the
IDLE pre-count check and CLAIM post-token check, the live Lua frame is the
one-result child, the selected continuation is in that child's bytecode, and
the parent snapshot footer encodes that exact child PC and frame-base delta.
The copied parent snapshot also contains one frame marker whose KNUM payload
matches the live child frame. The current bounded proof checks the root CALL,
callee identity, fixed argument vector, frame geometry, footer, and root loop
interval at these phases.

OWNER validation runs later, on recorder dispatch. The diagnostic trace starts
with the child identity (`J->pt`, `J->fn`, `startpt`, and `startpc` all identify
the child). As recording advances through the child return, the live Lua frame
and current bytecode move to the root caller's return continuation, while the
active trace's start identity remains the child. The child's represented
`framedepth` falls from one to zero; the observed `retdepth` remains zero.
Thus this is a recorder-time frame transition after the initial snapshot
restore, not a second snapshot restore and not evidence that the owner changed
traces. See `diag27.log` in
`/Users/frityet/Scratch/ljmt-seq-child-side-20261030/`.

The current OWNER preflight incorrectly reruns the IDLE physical-child-frame
reconstruction against the already-transitioned live stack. It then compares
the recorder's child start identity with `curr_func(L)` at the root
continuation. That fails closed, but cannot be repaired by treating any root
frame or root PC as equivalent to the child identity.

## Proof needed before admitting the transition

An OWNER-phase extension must keep the original child proof tied to the exact
parent trace generation, exit, snapshot frame marker, source CALL, callee, and
argument window. For the transitioned phase it must separately establish:

* the live root frame has the exact caller proto/function and return PC derived
  from that CALL;
* the active recorder still has the same child start proto/function and
  continuation, owner, parent, and exit;
* the recorder has represented the child-to-root return with the expected
  frame-depth transition, and the following root bytecode is the exact saved
  continuation;
* source replay, snapshot liveness/restoration, semantic admission, post-RA
  validation, and linking all support the resulting trace geometry.

The recording code emits `IR_RETF` and increments `retdepth` only when
`lj_record_ret()` returns to a lower frame not represented in the trace
(`framedepth == 0`). The observed path instead returns from a represented frame
(`framedepth` drops from one to zero), so the diagnostic does not show an
`IR_RETF` requirement. The implementation must still verify the actual recorded
IR and snapshot path before relying on that distinction.

The existing scalar side admission is single-proto: its common context requires
the trace start proto to equal `J->pt`, and its side-op prefilter does not
include `IR_RETF`. A complete cross-proto implementation therefore needs an
independent bounded child-to-root source/frame certificate in semantic and
post-RA paths; changing only the recorder preflight is insufficient. Until that
proof and native entry/exit/GC evidence exist, the safe result is refusal.

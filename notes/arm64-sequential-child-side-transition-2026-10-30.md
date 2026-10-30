# ARM64 child-frame side transition audit (2026-10-30)

This is a diagnostic checkpoint for the isolated `codex/arm64-seq-child-side`
worktree, based on `821fff928`. It does not claim child-frame side publication
or open the production publication boundary.

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

An OWNER-phase extension needs an explicit certificate with these fields (or
an independently checked derivation of each field):

| Certificate field | Required binding |
| --- | --- |
| Parent generation | Parent trace number and body identity, root number, exit number, trace-vector slot/generation, and active footer/map bounds. |
| Root source | Root proto, loop start/interval, exact root `BC_CALL` instruction and its PC. |
| Child source | Child proto, exact callable function object, fixed argument/result arity, and argument window. The function object must be recovered from a parent guard/source relation after the physical child frame is gone. |
| Entry frame | Child base/top, root caller base, frame delta, child return PC, exact frame-marker KNUM, snapshot number/ref/map range, and footer PC/base encoding. |
| Recorder identity | Owner/token, side trace number, parent/exit, child `startpt` and `startpc`, synthetic start instruction, and the represented `framedepth` transition. |
| Transition target | Live root function/proto, exact caller return PC, restored root base, and result slots corresponding to the original CALL. |
| Admission evidence | Semantic and post-RA source/snapshot proofs for both protos and their frame transition; the final root link and every cold exit must restore the same caller state without replaying the child call. |

At IDLE/CLAIM, the existing bounded proof can derive the parent generation,
source CALL/callee/arguments, and entry frame from the live child frame plus
immutable parent snapshot. At OWNER after return, the live stack proves the
transition target, while J proves the child recorder identity. The exact child
function object is no longer directly available from the child frame; it must
be independently recoverable from an immutable parent call guard or retained
in token-private recorder state that is initialized, revalidated, and cleared
with the recorder lifecycle. No acceptance path should infer it from only the
child proto, a reused caller slot, or the fact that arithmetic still executes.

For the transitioned phase the proof must separately establish:

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

## Captured return boundary checkpoint

The source/frame entry proof now checks the caller bytecode's exact fixed
one-result `CALL` using its separate LuaJIT B (result count) and C (argument
count) fields: this bounded case requires B=2 and C=2. It verifies the live
child frame's callable and argument window, the copied parent snapshot's
`SNAP_FRAME` KNUM payload, and the exact PC/base footer before allowing the
IDLE/CLAIM entry checks to pass. The full caller bytecode is scanned for
invalid jumps and jumps that could enter after the captured upvalue load and
bypass the call target definition.

At `lj_record_ret`, a token-private child-return certificate is captured only
for a represented one-result pure leaf returning through that same exact
CALL. It records child/caller function and proto identity, child RET1 PC,
caller CALL/return PCs, parent generation/trace/exit identity, result refs,
and the split IR position. That record is evidence only: it is not yet consumed
by side-owner or semantic/post-RA admission, and it does not authorize
publication. The ASM owner still requires the existing single-proto source and
link shape, so the ordinary reachable same-SSA probe continues to publish no
child side on ARM64. x64 is only a compile-time control for this ARM64 fixture.

The focused ARM64 assertion fixture passed on Xcode clang with
`MacOSX27.2.sdk`: native=34, child exits=10, same-trace reuse=24, GC reuse=7,
semantic negatives=392, post-RA negatives=421, and one captured return
certificate. The x86_64 assertion build and fixture skip also passed; it does
not validate ARM64 behavior. Logs are
`/Users/frityet/Scratch/ljmt-seq-child-side-20261030/arm64-capture-final.log`
and `x64-compile-check.log`. The earlier MacOSX27.0 SDK resolution failure is
toolchain evidence only and is superseded by these SDK27.2 runs.

## Constant-reference boundary review

An independent review found that the guarded-call proof read KGC and KINT
references by numeric range alone. It now walks the addressable constant table,
skipping paired payload words and requiring each pair to fit below both
`REF_TRUE` and the captured IR extent before the production proof dereferences
the selected constant. A focused fixture checks valid KGC/KNUM/KINT heads,
rejects paired payload aliases, and rejects a KGC head at `REF_TRUE-1` whose
payload would cross the constant boundary. The
four-profile Lua leaf contract passed again on Xcode clang with SDK 27.2;
its durable log is
`/Users/frityet/Scratch/ljmt-seq-child-side-20261030/constant-bounds-final-four-profile.log`.

The strict Xcode clang x86-64 Darwin build also passed and produced an x86_64
binary; its log is
`/Users/frityet/Scratch/ljmt-seq-child-side-20261030/x64-constant-bound-build.log`.
The new constant walker is within the ARM64 experimental section. An earlier
x86-64 build attempt failed before compilation because the Makefile selected
the host architecture before the target SDK and deployment variables were set;
that failed attempt is superseded by the recorded build. The temporary OWNER
diagnostics were removed after capturing the return transition, and ordinary
execution still demonstrates root-only publication. No side publication is
enabled by this follow-up.

The certificate now also carries the current side trace number, and every later
`lj_record_ret` clears the preceding witness before considering a new return.
This prevents a transition record from being borrowed by another trace or
surviving a second return in the same recording. The copied child-frame marker
now uses the same addressable KNUM-head proof before reading its payload.
The follow-up four-profile leaf run passed with the same native, exit, reuse,
GC, semantic-negative, post-RA-negative, and certificate counts; log:
`/Users/frityet/Scratch/ljmt-seq-child-side-20261030/child-return-trace-binding-four-profile.log`.

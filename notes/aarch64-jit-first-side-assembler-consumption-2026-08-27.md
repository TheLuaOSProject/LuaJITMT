# AArch64 first-side assembler certificate consumption (2026-08-27)

## Boundary

This checkpoint lets the production ARM64 assembler consume the already
proved first-side semantic, parent-lifetime and post-RA certificates. It does
not open side recording: `LJ_ARM64_JIT_SIDE_RECORDER_FAIL_CLOSED` remains `1`.
There is no production `trace_hotside()` admission, `trace_stop()` publication,
parent exit retarget, topology update or retirement change. A side trace
therefore still cannot reach this path from production ingress. A dedicated
`LJ_ARM64_SIDE_ASM_TEST` build provides the only one-shot test bypass; that
macro requires trace test helpers, the experimental macOS ARM64 JIT and a
still-closed production side recorder at compile time.

## Transaction order

For an exact nonzero `J->parent`, `lj_asm_trace()` now requires `J->loopref ==
0`, constructs the canonical `LJArm64SideIRView`, and runs
`lj_asm_arm64_side_ir_admit()`. Only after semantic success does it capture the
eight-field token-private parent/destination certificate. Capture precedes IR
growth, scratch trace allocation, mcode reservation and exit-table setup.
`RETRY` maps to `LJ_TRERR_RETRY`; a closed one-shot SMR admission maps
distinctly to `LJ_TRERR_SMRRETRY`.

`trace_abort()` clears the embedded certificate before mcode teardown. Thus an
`LJ_TRERR_MCODELM` retry re-enters `lj_asm_trace()` and must repeat semantic
admission and capture rather than reuse an earlier generation.

ARM64 side assembly obtains:

- `ASMState.parent` only from certified `body`;
- the selected parent exit only from certified `exitno`; and
- a linked-tail target only from certified raw `mcode`.

It does not reacquire a side parent through `asm_traceref_live()` or
`traceref()`. Unexpected ARM64 non-loop links without an exact certified parent
fail with `RETRY`; root loops retain their existing `asm_loop_tail_fixup()`
path, while interpreter tails retain `link == 0`.

## Parent-map and post-RA consumption

The parent certificate now also proves that the selected, bounded and sorted
parent snapshot map contains canonical slot 4. Exact side semantics prove this
is the sole `IRSLOAD_PARENT` requested by the child. Revalidation immediately
before `asm_setup_regsp()` therefore makes the otherwise assertion-only
`lj_snap_regspmap()` slot search bounded in release builds.

At the top of `asm_head_side()`, before its parent IR reads or parent-map
indexing, the assembler revalidates again, requires exact body identity, and
runs the pure `lj_asm_arm64_side_prehead_admit()` layout certificate. The view
uses `J->curfinal->nins` as the allocator-visible compact extent because it owns
the trailing NOP while each pass temporarily resets `J->cur.nins` to the
semantic boundary. The pre-head half checks the entire canonical semantic IR,
allocator registers/spills, stack adjustments, top slots, trailing NOP and the
single x28 parent-map value, but deliberately does not inspect entry bytes that
have not been emitted yet.

After assembly settles and optional BTI emission is complete, the full
`lj_asm_arm64_side_postra_admit()` reruns that layout certificate and proves the
emitted `BTI J` (when configured) plus `MOV x27, x28` prefix. Only this exact
success can eventually produce `TRACE_ARM64_INT_SIDE_ADMITTED`.

## Tail and final seam

Immediately before linked-tail finalization, the assembler revalidates the
certificate, requires `T->link == cert.parent` and `ASMState.parent ==
cert.body`, then passes `cert.mcode` directly to the ARM64 B26 fixup. The tail
helper contains no trace-slot lookup.

`asm_snap_fixup_mcofs()` remains the last fallible assembler operation. After
it succeeds, the certificate is revalidated once more. The scratch-only side
admission marker is set only then, after both exact post-RA success and final
parent revalidation, and before the no-throw mcode fixup/cache-sync seam. An
abort therefore cannot leave an admitted bit for a later mcode retry.

## Executable contracts

The focused side-IR fixture directly tests the split pre-head helper: every
semantic/layout/map mutation is rejected, while entry pointer, BTI and MOV
mutations are intentionally ignored pre-head and rejected by the full helper.
The parent metadata fixture changes/removes slot 4 and proves capture and
revalidation fail without leaking an SMR reader. CI pins the exact production
capture/revalidation counts and ordering, compact-extent authority, certified
body/exit/mcode sources, tail `traceref()` ban, final marker seam and the still
closed recorder macro.

The dedicated native consumption fixture arms only parent 1, exit 2. Its
test-only `trace_hotside()` bypass and START/RECORD preflight both require the
dedicated macro and repeat the exact owner, generation and first-side metadata
proof. After assembly and mcode cache synchronization, every special-build
side attempt throws before `trace_stop()` regardless of diagnostic state, so
the test build cannot publish or retarget a child.

The fixture forces one exit-stub `LJ_TRERR_MCODELM` restart and proves two
certificate captures but only one completed assembly. Each capture now pins the
exact TraceVec generation and reserved `LJ_TRACE_PENDING` child slot as well as
the parent. It observes the real
single x28 parent map, optional `BTI J`, `MOV x27, x28`, the certified parent
target and the actual emitted B26 instruction, final revalidation and the
scratch-only side marker. The mandatory post-assembly abort then proves slot 2,
child topology, snapshot DONE, parent exit retarget and side publication all
remain absent; recorder state, token, owner, certificate, SMR readers, cframe,
VM state and `jit_base` all return cleanly, and root 1 remains natively usable.

`tools/ci/arm64_jit_side_asm_consumption_contract.sh` rebuilt and executed this
path twice as ordinary arm64 and twice as arm64e with BTI/PAUTH on this machine.
All four executions passed, including the forced retry and the later dry
publication seal, and the runner restored an ordinary arm64 helper build
afterward. The ordinary build is checked to contain none of the special probe
or dry-seal API symbols.

The complete `arm64_jit_fail_closed_gate.sh` umbrella also passed after this
integration, covering both arm64 and arm64e native paths together with root
entry, exits, trace retirement, live flush/reuse and safepoint runtime checks.

The later dry-seal checkpoint proves the same-word abort race and exact
pre-publication inputs at this seam, but still performs no irreversible action.
It does not prove child publication, authenticated parent-exit publication,
enterability or side/root retirement. Those remain separate transactions that
must be completed before the recorder gate can open.

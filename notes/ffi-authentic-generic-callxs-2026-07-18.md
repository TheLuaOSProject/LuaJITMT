# Authentic generic scalar CALLXS checkpoint (2026-07-18)

## Status and scope

This checkpoint executes the real generic x64 `IR_CALLXS` seam between the
XSAVE-consuming exact-frame entry/leave helpers. Activation is deliberately
test-only under `LJ_FFI_CALLXS_TEST_ACTIVATE`; the default recorder still raises
`LJ_TRERR_BLACKL`. No `plan/` file is changed.

The path is declaration-independent. It adds no signature enum, C declaration
matcher, wrapper catalogue, or per-shape dispatcher. CType-driven argument
conversion and the existing x64 ABI classifier continue to build one generic
`CARG` tree and lower one `CALLXS`.

## Generated lifecycle

For the currently admitted nonallocating scalar result-handoff classes,
recording builds every CType argument conversion before the native boundary,
then emits:

```text
XSAVE
CALLS lj_ffi_native_trace_enter(L, exact_trace, raw_function)
guard enter != 0                 -- pre-call/replaying snapshot
CALLXS generic_CARG_tree
pure result normalization
caller-state snapshot
CALLS lj_ffi_native_trace_leave(L)  -- first C call after the foreign call
guard leave == 0                 -- post-call/non-replaying snapshot
```

The admitted result normalizations are nonthrowing machine conversions and do
not alter the foreign error pair. Leave remains the first C call after the
foreign return. Its `CCI_T` unwind and its forced guard now share the caller
snapshot because that snapshot's IR reference names leave itself. The enter and
leave IRCALL descriptors intentionally do not claim preserved FP registers;
normal register allocation must move or spill the normalized result across
leave. The authentic matrix proves both GPR and XMM result survival.

Both native-boundary guard snapshots carry `SNAPCOUNT_DONE`, used here as an
explicit never-record marker. LOOP copy-substitution preserves this marker.
The pre-call rejection stays in the interpreter, while the completed-call exit
must pass through central trace-exit cleanup; linking either exit to a side
trace could otherwise cross or bypass native lifecycle ownership.

The opt-in boundary currently accepts ordinary `CALL`, `CALLM`, and `ITERC`
Lua frames only. Bool, pointer, enum, i64 and u64 results stay interpreted:
their current result paths branch or allocate cdata after the foreign side
effect. Protected, continuation, vararg-frame and tail-return shapes also stay
interpreted until each has an exact post-return snapshot contract. These are
return/frame-class safety boundaries, not declaration matching.

## Exact trace constant on loop traces

The first authentic loop showed that KTRACE patching in `asm_tail_link()` was
not sufficient: normal loop traces skip tail linking, so their placeholder
remained null and native entry rejected every execution. That looked correct at
the Lua level because the pre-call exit replayed the call in the interpreter.

KTRACE is now patched immediately after each assembly attempt selects the
copied `J->curfinal->ir`. This covers loop and non-loop traces plus IR-growth,
alignment and RENAME retries which allocate a fresh final body. The generated
fixture inspects the enter CARG and proves its KGC is the exact finalized body.

## Caller-state post-call snapshot

The first forced leave exit exposed a real continuation bug. `lj_record_ret()`
had already moved `J->base`, `baseslot`, `framedepth` and result slots back to
the Lua caller, but `L->base/top` and `J->pc/fn/pt` still described the live
fast-function retry. The snapshot therefore combined caller values with the
synthetic `BC_FUNCC` PC. Its side trace started at source line zero and restore
resumed `lj_BC_FUNCC` with a Lua closure where a C closure was expected,
jumping through a Lua upvalue word into non-executable GC memory.

`ffrecord_postcall_snap()` now presents the matching Lua caller view only while
the snapshot is built. It does not shift the recorder stack a second time. It
validates the caller PC against the caller prototype, snapshots with caller
`L->base/top` and `J->pc/fn/pt`, and then restores the still-live fast-function
view. Terminal trace links and unsupported physical frame shapes fail recording
instead of appending a guard to an invalid continuation.

For its i32 side-effect target, the C fixture mechanically verifies that every
activated CALLXS is immediately followed by native leave, every leave guard
owns a snapshot inside the expected Lua prototype, and the root snapshot's
frame/top encoding matches that prototype. It then forces a handshake-epoch
change while the final frame sequence is odd. The real guard restores the
caller, releases the retained pin once, preserves the exact foreign side-effect
count, and leaves the root safely re-enterable.

## One-shot XSAVE ownership

Every native-entry attempt consumes all three XSAVE staging words. Earlier
rejected entries left them intact for the pre-call snapshot exit, but a hot
exit can link directly to side code and bypass central trace-exit cleanup.
Authentic occupied-depth injection exposed the stale raw
`ffi_xsave_root/baseslot/nslots` geometry after interpreter fallback.

`lj_ffi_native_trace_enter()` now performs the complementary owner clear on
every valid-carrier return, including capacity, occupied-depth, malformed
geometry, trace-pin and publication rejection. This keeps dormant XSAVE-only
instrumentation independent of unrelated trace exits. The injected rejection
proves that CALLXS and generated leave do not run, the interpreter performs each
foreign side effect exactly once, and all staging is zero afterward.

## Deterministic evidence

`m7_ffi_callxs_authentic` builds the runtime with the explicit activation macro
and runs two fixtures:

- `t-ffi-callxs-authentic.lua` covers void, signed and unsigned 8/16/32-bit
  scalar returns, float, double, mixed GPR/XMM/pointer/u64 arguments, C
  varargs, and generated-call `errno` preservation. It requires real XSAVE and
  CALLXS IR, while proving bool, pointer, enum, i64 and u64 results remain
  interpreted with their exact values intact.
- `t-ffi-callxs-postcall.c` proves exact finalized KTRACE identity, immediate
  leave ordering, caller snapshot PC/frame geometry, permanent no-side-trace
  markers, repeated forced POSTCALL cleanup under `hotexit=1`, exact side
  effects, a deterministic fresh-STOPREQ injection between generated native
  leave and its throw decision from the same caller snapshot, re-entry,
  rejected-entry fallback, zero native depth, zero frame depth, zero trace pins,
  restored callback/function mirrors and cleared XSAVE staging.

Focused validation passed:

- `m7_ffi_callxs_authentic`;
- default-gated `m7_ffi_ccall_native` and `m7_ffi_native_frames`;
- `m6_jit_xsave`;
- `m5_jit_trace_publish`;
- Clang 19 ASan with fail-fast and leak checking disabled for both authentic
  fixtures;
- Clang 19 UBSan with the repository's documented alignment, function,
  pointer-overflow and shift exclusions;
- default, `LUAJIT_DISABLE_JIT`, and `LUAJIT_DISABLE_FFI` builds.

## Production gate remains closed

This checkpoint does not claim safe default generic FFI tracing. The remaining
coupled gates are:

1. callback `ACTIVE -> SUSPENDED -> ACTIVE` publication, nested callback and
   error-unwind cleanup, plus nonwaiting carrier leasing;
2. pre-rooted boxed result storage and nonthrowing bool/result handoff;
3. exact snapshot contracts for pcall, continuation, vararg and tail/RETF
   return shapes;
4. remote trace-flush admission for a certified pinned ACTIVE frame, followed
   by forced POSTCALL exit from the retired exact body;
5. descriptor-driven aggregate argument and multi-register result lowering;
6. Linux stress and sanitizer proof followed by Win64/Wine and macOS/Darling
   ABI and error-state coverage.

Unknown foreign functions can callback on their first invocation regardless of
their declaration, so historical callback blacklisting cannot justify opening
the default gate. The next lifecycle tranche is callback suspension, not a new
signature allowlist.

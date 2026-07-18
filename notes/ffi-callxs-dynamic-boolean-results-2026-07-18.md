# Replay-safe dynamic boolean CALLXS results (2026-07-18)

## Status

The production generic x64 FFI recorder now admits C `_Bool` results to the
same declaration-independent `IR_CALLXS` path as the existing scalar and
rooted boxed classes. This supersedes the boolean exclusion recorded in
`ffi-callxs-rooted-boxed-admission-2026-07-18.md`; that note remains the
certificate for the preceding commit. Aggregate and multi-register results,
and the broader protected/continuation/root-tail frame shapes, remain future
work. No file under `plan/` was changed.

There is still one generic ABI-driven call recorder and one CALLXS lowering.
Boolean admission does not add a declaration catalogue, signature enum,
per-function wrapper, or an explicit C-call shape matcher.

## Why the result cannot be specialized immediately

The trace recorder observes a C call before the interpreter executes that
particular invocation. Consequently the interpreter's `tmptv2` boolean still
describes an earlier operation while `crec_call()` is running. Reading it there
would specialize the trace with a stale value.

The foreign result also exists before native leave has captured error state,
processed callback/remote requests, and either closed or transferred the
published native frame. A boolean mismatch guard placed before leave could
exit with an ACTIVE frame, while a constant-valued pre-leave snapshot could
restore the wrong value if leave itself throws or forces an exit.

## Marked-result protocol

For the standard one-byte and four-byte boolean representations, recording is:

```text
CALLXS with the raw U8/U32 result
CONV to INT marked IRCONV_BOOL
move the marker into the reconstructed Lua caller result slot
DONE post-call snapshot containing the marker
CALLS lj_ffi_native_trace_leave
guard leave_flags == 0
prepare guard marker != 0
provisionally replace the result slot with true
defer guard direction selection to LJ_POST_FIXGUARD
```

No IR is emitted after preparing the pending boolean guard in that recorder
turn. On the next recorder turn the interpreter has published the current
call's real boolean in `tmptv2`; normal `LJ_POST_FIXGUARD` handling selects
`NE` for true or `EQ` for false, emits the guard, and changes the provisional
slot to false when required. Later IR therefore sees an ordinary Lua boolean
constant, while the native result remains dynamic until cleanup is complete.

The marker-bearing post-call snapshot is deliberately `SNAPCOUNT_DONE`. A side
trace must not inherit its temporary integer IR model while snapshot restore
produces a Lua boolean, and it must not bypass retained POSTCALL cleanup. If the
result is discarded, the marker has no caller slot and no value guard is
needed. Used results must have one unique caller slot; ambiguous aliases fail
closed to the interpreter.

## Exit restoration

Snapshot restore recognizes only `IR_CONV` instructions carrying
`IRCONV_BOOL`. It normalizes the saved integer with `value != 0` and writes a
real Lua boolean for all three allocator shapes:

- a spill slot;
- a general-purpose register;
- a conversion whose source must be restored recursively because the marker
  itself has no assigned register.

Thus the same marked snapshot is correct for every post-side-effect exit:

- native leave throws through `CCI_T`, including a fresh STOPREQ;
- native leave requests a forced exit after a callback, GC action, or flush;
- leave succeeds but the runtime boolean differs from the recorded
  specialization;
- both guards pass and subsequent trace IR uses the specialized true/false
  constant.

The conversion CSE key distinguishes marked boolean conversions from ordinary
integer conversions, so an optimizer cannot erase the snapshot semantics.
The marker remains live across native leave because the following value guard
uses it; register allocation therefore preserves it in a register or spill.

## Compatibility and cost

Lua receives an actual boolean for both generated and interpreted calls. The
public LuaJIT API, ABI, CType identity rules, and native calling convention are
unchanged. Non-x64 targets and nonstandard boolean sizes still use the exact
interpreter fallback.

The steady generated path adds one integer conversion and one post-leave value
guard. It performs no allocation, lock acquisition, signature dispatch, or
helper call for boolean normalization. Native leave remains the first helper
after the foreign return, preserving the existing errno/LastError contract.

As authorized for the beta line, custom `lua_Alloc` is still temporarily
ignored by the GC2/internal allocator policy. This nonallocating boolean
change neither expands nor resolves that documented exception.

## Evidence

- Authentic production traces mechanically require CALLXS for both true and
  false results and require the exact Lua type `boolean`. An ABI-compatible
  byte-return twin also supplies noncanonical nonzero bits, which normalize to
  true on the generated path.
- Traces warmed true and false are run with the opposite result in both
  directions with exact native effect counts, proving the completed foreign
  call is not replayed.
- Bytecode-authenticated CALL, CALLM, CALLT, CALLMT, and ITERC forms cover the
  admitted physical caller topology.
- Discarded, single, excess fixed, and open result modes are covered.
- IR inspection proves `CALLXS -> IRCONV_BOOL -> DONE snapshot -> leave ->
  force guard -> boolean guard`, with exactly one marker reference in the
  post-call snapshot.
- Forced POSTCALL exits restore an opposite runtime value and release the
  frame, trace pin, and native state exactly.
- A fresh STOPREQ thrown inside native leave exercises the `CCI_T` restore and
  central unwind path without call replay or retained lifetime leaks.
- Remote full GC and `jit.flush()` both finish while a boolean CALLXS is
  blocked; after retirement the call returns the opposite boolean and its
  side effect occurs exactly once.
- A generated Lua callback suspends the boolean native frame, sets
  `CALLBACK_SEEN`, returns the opposite specialization, and leaves all callback,
  frame, pin, and XSAVE state empty.
- The focused authentic, remote-flush, POSTCALL, and callback fixtures pass in
  clean default, AddressSanitizer, and fail-fast UndefinedBehaviorSanitizer
  target builds. `LUAJIT_DISABLE_JIT` and `LUAJIT_DISABLE_FFI` feature builds
  also compile and link.

## Next work

The next FFI/JIT tranche is generic ABI lowering for aggregate arguments,
aggregate/complex/vector and multi-register results, plus exact reconstructed
post-return views for the remaining protected, continuation, direct-vararg,
root-tail, and terminal frame shapes. Ordinary CType readers and same-callback
carrier admission must also lose their waits. None of those extensions may
reintroduce declaration-specific call-shape matching.

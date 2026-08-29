# AArch64 FFI callback result lifetime (2026-08-28)

## Failure and claim boundary

A TLS-less foreign callback auto-attaches its hidden carrier state to a heap
TG. Result conversion writes the native return carriers into that TG's
`CCallbackRuntime`. The inherited x64 continuation, and the first ARM64 port of
it, called `lj_ccallback_leave()` before reloading those carriers. Normal leave
could detach the TG, drop the final `mt_live` token, and publish the TG dead.
The runtime-main TG could then physically reclaim the dead TG while the
assembly continuation was still about to read `cb.gpr[]` or `cb.fpr[]`.

This checkpoint closes that post-detach use-after-free window for the supported
ARM64 and GC64 x64 callback backends. It does not widen the callback type ABI:
the existing `callback_checkfunc` scalar-only limit remains unchanged. Legacy
backends retain their old eager-leave path instead of acquiring a debt they do
not know how to finish.

## Lifetime protocol

`lj_ccallback_leave_result()` performs the existing result conversion, frame
pop, callback publication cleanup, and native-depth restoration. For an
ordinary attached callback it returns `NULL`. For a foreign auto-attached
callback it republishes the exact `ccallback_auto_detach == 1` bit as a
result-reload lifetime debt and returns the carrier `lua_State *` without
detaching.

The assembly continuation then copies every previously supported return
carrier out of TG-owned storage:

- ARM64 uses saved `x19`/`x21` and the low 64-bit halves of saved `d8`/`d9`;
- SysV x64 uses saved `r15`/`rbx` through the `KBASE`/`PC` aliases; and
- Win64 x64 uses saved `rdi`/`rsi` through the same aliases.

If leave returned a carrier, the continuation calls
`lj_ccallback_leave_result_finish()`. The finish helper preserves the selected
`errno`/Win32 `LastError` pair, performs the exact pending detach, and then
touches no `L`, TG, global state, or callback storage. Assembly restores the
integer and floating-point result registers only after finish returns and then
uses the ordinary VM epilogue.

`lj_threading_detach_callback_pending()` is shared by normal result completion
and terminal callback unwind. It is deliberately fail-stop rather than a
retryable public detach. It requires the exact secondary TG/current-state pair,
zero callback depth, no callback carrier/slot/STOPREQ/FFI-call publication, and
the debt bit exactly one. It clears only that bit, runs the unchanged scope and
JIT-detach readiness predicates, and commits detach. Any invariant split
restores the debt before aborting.

## Deterministic regression proof

`t-ffi-callback-auto-attach.c`, when built with
`LJ_CCALLBACK_TEST_HELPERS`, installs an atomic one-shot hook immediately after
detach and before the helper returns to assembly. The foreign callback blocks
there while the runtime-main TG proves `mt_live == 0` and physically reclaims
the sole dead TG. It then releases the continuation and verifies:

- the complete `0xfedcba9876543210` integer carrier survives;
- the independent floating-point carrier returns `13.0`;
- TLS remains clear after callback return; and
- `E2BIG`/`EILSEQ` survive a hook that deliberately writes `ERANGE`.

`tools/ci/arm64_ffi_callback_result_lifetime_contract.sh` pins the source
ordering and every pending-detach predicate, builds ARM64, arm64e/BTI, and
x86_64 variants, and runs both reclaim cases twice with `MallocScribble=1`.
It also pins the distinct SysV/Win64 saved-register layouts and the Win32 error
pair in source. Dynamic Win64 and `LastError` execution remain a Windows-host
validation item.

## Validation

The following passed on the Apple Silicon host:

- the callback-result lifetime contract on ARM64, arm64e/BTI, and Rosetta
  x86_64, two scribbled-reclaim runs per architecture;
- the independent arm64e PAUTH/BTI callback create/call/free/reuse contract;
- ARM64 nested callback, owner-lifetime, STOPREQ, attached-carrier, normal
  auto-attach, and C++ exception-unwind/detach fixtures;
- the complete ARM64 experimental JIT umbrella, including GDBJIT, exact first
  sides, native LOOP/FORL/JFUNCF, exits, retirement, flush/reuse, and
  safepoints;
- the macOS x86_64 platform smoke and all callback lifecycle fixtures,
  including real C++ exception unwind; and
- the x86_64 vendored stock suite: 509 passed.

The existing `lj_ccall.c` unused-helper warning and x86_64 `lj_api.c` `topofs`
warning were unchanged.

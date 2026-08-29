# Auto-attached FFI callback error unwind

## Bug

TLS-less foreign callbacks acquire a secondary TG and one `mt_live` universe
lifetime token in `lj_ccallback_prepare()`. Normal callback return removes the
callback C frame before `lj_threading_detach()`, but the error path historically
called the same public detach from `lj_ccallback_unwind()` while
`L->cframe != NULL`.

The public detach gate now correctly treats every C frame as a retryable scope
veto. Consequently that error-path call became a deterministic no-op: callback
roots were cleared, but TLS, state ownership, the TG registry entry, and
`mt_live` remained attached forever. Weakening the public gate would permit
ordinary API callers to release the state/universe lifetime while protected VM
frames still carry raw pointers.

## Unwind-owned completion

`lj_ccallback_unwind()` now pops the callback/native publications but preserves
the outer frame's `auto_detach` bit as an exact one-bit lifetime debt. It does
not detach while the logical C frame remains installed.

The platform error unwinder consumes that debt only after `err_unwind()` has
removed the logical FFI continuation and returned NULL for the unprotected
callback boundary:

- Win64 SEH consumes it in the unwind phase immediately before
  `ExceptionContinueSearch`;
- DWARF consumes it only on paths proven to return `_URC_CONTINUE_UNWIND`,
  after excluding every `INSTALL_CONTEXT` and ancient-libgcc rethrow landing
  which could still use `SAVE_L`;
- ARM EHABI consumes it before advancing the physical frame with
  `__gnu_unwind_frame`.

This is the last LuaJIT access to `L`/`g` before the platform unwinder disposes
the physical callback frame. It is deliberately not driven by
`_Unwind_Exception.excleanup`: LuaJIT's own x64 exception normally is not
deleted on its landing path, while a foreign exception may be deleted before
the cleanup-phase callback continuation has been processed.

The internal `lj_threading_detach_callback_pending()` path is narrower than the
public API. It requires the exact current secondary TG/state pair, callback
depth zero, no callback carrier/slot/native-stop snapshot/FFI call root, and the
pending bit exactly one. It clears only that bit, then reuses the unchanged
public C-frame/native/event/JIT readiness checks and the common detach commit.
An unexpected refusal is fail-stop because there is no lawful actor left to
retry after the physical unwind continues.

Both callback unwind and its final detach helper preserve the selected
errno/Win32 LastError pair around all lifecycle work. The outgoing pair remains
the body throw-edge value or the setup/result snapshot selected by the callback
frame; registry retirement, futex wakes, and TG cleanup cannot replace it.

At the time of this checkpoint normal callback return still used the public
detach path. The 2026-08-28 result-lifetime hardening now uses the same exact
pending-detach helper for supported ARM64/GC64-x64 auto-attached returns, but
only after assembly copies TG-owned result carriers into ABI-preserved
registers. Attached callbacks never publish the debt, nested callbacks
propagate only the outer auto-attached frame's bit, and legacy backends retain
eager public detach. See
`../aarch64-checkpoints/aarch64-ffi-callback-result-lifetime-2026-08-28.md`.

## Regression contract

`t-ffi-callback-auto-unwind.cpp` invokes a throwing Lua callback from a raw
foreign pthread and catches the LuaJIT exception through a real C++
`catch (...)`. The inspect phase holds a GC2 SMR lease while checking the
retired raw TG, then proves TLS is NULL, state ownership and `tg_hint` are
cleared, callback publications (including the debt bit) are empty, `mt_live`
returns to baseline, and deterministic dead-TG reclaim succeeds.

The lifetime phase holds the C++ catch open while the main owner enters
`lua_close()`. A bounded releaser lets the catch exit later; close must remain
protected by the still-live auto-attach token and completes only after unwind
detach releases it. That phase intentionally takes no extra SMR lease, since an
auxiliary lease would mask the lifetime-token proof.

Linux/x86-64 DWARF is the release validation target for this tranche. The code
contains corresponding Win64/ARM placements, but Wine/Darling and non-external
unwind matrix coverage remain part of the deferred cross-platform validation.
An unhandled error in a non-external-unwind build terminates from inside the
stack walk and is not a recoverable foreign-catch/auto-detach contract.

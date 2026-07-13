# FFI callback carrier root retirement (2026-07-13)

## Failure

The exact b1.2.0 release-candidate run found that
`t-ffi-callback-auto-attach` could consume one CPU indefinitely in the final
`collectgarbage("collect")`. The collector repeatedly reached the same
quarantined arena with one `lua_State` in `RETIRED`, one deferred destructor,
and no changing producer state.

Symbolized inspection identified the state as the hidden callback carrier.
Each TLS-less callback had auto-attached a heap TG and detached it normally.
The TG correctly published `TGF_DEAD` after clearing `cur_L` and `thread_L`,
but its zero-depth `CCallbackRuntime.L` still named the carrier. Terminal
THREAD preflight treats every active callback publication as a raw state root,
so the stale field vetoed the destructor on every bounded reclaim pass. The
arena cursor reached EOF, observed the unchanged deferred count, restarted,
and made an unbounded sequence of nominal cursor progress.

## Contract and fix

`CCallbackRuntime.L` is a transient root from callback preparation through the
last active callback frame. It is not a cache and must not remain published at
depth zero.

- Normal callback frame pop now release-clears `cb.L` when it removes the last
  frame.
- The setup-error unwind path clears `cb.L` when no frame was published.
- TG detach requires zero callback depth and defensively clears the carrier,
  slot, auto-detach, and saved STOPREQ publications before publishing `DEAD`.

Nested callbacks keep `cb.L` until their outermost frame leaves. Frame slots
remain the authoritative per-depth roots, so clearing only at depth zero does
not weaken an active callback lifetime. TG detach remains fail-stop if an
active frame reaches it.

## Regression coverage

The auto-attach fixture now inspects retained dead TGs after foreign-thread
joins and rejects a stale callback-carrier publication. Its suite entry also
has a 20-second timeout so any future collector progress failure is bounded.
The existing final full GC remains the end-to-end proof that both carrier
states become reclaimable after their callback slots are freed.

Validation on the candidate fix:

- default `m7_ffi_callback_runtime`, including nested, owner-lifetime,
  STOPREQ, attached-carrier, auto-attach, Lua runtime, and stock callback
  fixtures;
- 20 consecutive optimized `t-ffi-callback-auto-attach` runs;
- `LUA_USE_ASSERT + LJ_GC2_PARANOIA=1`, `-O0 -g3` focused auto-attach run.

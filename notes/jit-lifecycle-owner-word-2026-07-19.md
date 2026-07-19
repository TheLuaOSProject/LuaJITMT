# Atomic JIT token/lifecycle owner word (2026-07-19)

## Purpose

Token-free JIT VM events cannot be implemented by releasing the recorder token
and later reacquiring it: another recorder could claim the mutable universe JIT
state between those two operations.  The first event-session prerequisite is
therefore an indivisible ownership transition which keeps the current recorder's
lifecycle reservation while making the ordinary token visibly free.

This change installs that prerequisite only.  VM events do not use it yet.
Wiring it into callbacks before every waiting JIT control path understands the
reservation would turn those paths into unbounded waits.

## Representation and compatibility

`global_State` now contains one aligned 64-bit `jit_owner_word`:

- low 32 bits: ordinary recorder-token TG id;
- high 32 bits: token-free callback-lifecycle TG id.

Only these stable states are currently valid:

- `{0, 0}`: unowned;
- `{T, 0}`: TG `T` owns the recorder token;
- `{0, T}`: TG `T` owns a callback lifecycle reservation.

`{T, T}` is deliberately unsupported.  Ordinary token claim and release use an
exact whole-word CAS, so neither can enter or erase a live lifecycle state.
Lifecycle yield and resume use the exact transitions `{T,0}->{0,T}` and
`{0,T}->{T,0}` and also require the exact owning `lua_State`.

The qword occupies the former adjacent `jit_token` and
`jit_mcode_synccore` bytes.  `jit_mcode_synccore` moved into the existing four
bytes of padding after `gcroot_pending_hint`.  Static layout assertions preserve
`main_tg` and all downstream GG/J/dispatch offsets; the universe-global
structure size is unchanged.  The x86-64 build lowers the operation to native
`lock cmpxchgq` and has no unresolved 8-byte atomic runtime helper.

An IDLE-state lifecycle is intentionally permitted for a future token-free
`TRACE flush` event.  VM teardown now treats either half remaining nonzero as a
hard universe-lifetime violation.

## Tests and current boundary

The recorder-token fixture proves:

- exact token-to-lifecycle-to-token round trip;
- both ambient and explicit token tries fail while the high half is live;
- ordinary release cannot erase the high half or detach its `lua_State`;
- the final release returns the complete word and owner pointer to zero.

Fixtures which synthesize a foreign recorder now publish the complete word.
The MARK-cooperation fixture keeps a local full-word setter because it is built
both with and without test-helper symbols.

Focused validation passed clean `-Werror` builds of `m6_jit_token`,
`m6_jit_gc2_readiness`, and `m6_dispatch_redispatch`, plus a no-JIT feature
build.  The source gate remains green.

## Required follow-up

Before the transition can surround real callbacks:

1. token-waiting JIT control/flush/GC2 callers must recognize a lifecycle
   reservation and defer or enqueue instead of spinning;
2. each TG needs a rooted fixed session record and a frozen callback view;
3. attachment generations and pending-terminal state must preserve stock event
   pairing and handler replacement semantics;
4. callback completion must resume the exact lifecycle owner or perform a
   terminal unwind which clears it exactly;
5. tests must prove globally non-interleaved event streams, token-zero callbacks,
   nested-event suppression, handler error/detach/replacement behavior, GC
   visibility, STOPREQ behavior, and peer-control progress.

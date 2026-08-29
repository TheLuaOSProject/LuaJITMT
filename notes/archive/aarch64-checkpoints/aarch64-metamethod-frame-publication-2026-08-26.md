# Apple ARM64 metamethod-frame publication (2026-08-26)

## Claim boundary

This checkpoint makes the C helpers used by the ARM64 interpreter publish the
manual call frames for arithmetic, length, comparison/equality, concatenation,
and callable-object metamethods. It also publishes the freshly allocated
string written by the direct concatenation path.

It does not yet make metatable discovery itself generation-safe. The ARM64
`getmetatable` fast function and distinct table/userdata equality still require
rooted helpers, and `setmetatable` still needs to leave its unsafe inline fast
path. `ISNEXT` bytecode mutation also remains open. Safepoint acknowledgement
and the JIT remain disabled.

## Stack publication ordering

`lj_state_stack_pubtv` now performs its release self-store before incrementing
the owning TG's stack-dirty epoch, then runs the GC root barrier. This ordering
is part of the helper contract: many C callers first make an owner-private
plain store and rely on the helper's release self-store as the publication
point. Dirtying first would expose a window in which a scan certificate had
been invalidated before the completed `TValue` was visible.

The dirty counter remains the fork's relaxed certification counter. Authority
and parked/native-scan rules prevent it from acting as a general seqlock; this
change restores the already-documented store-before-invalidation order rather
than claiming a new concurrent scanner protocol.

## C-built frames

`mmcall` now records the method slot and publishes the method plus both
arguments after the complete frame has been assembled. Publication is owned by
`mmcall` itself, so the rooted table metamethod wrapper no longer repeats the
same three dirty/barrier operations before releasing its private anchors.

The manual `__concat` frame retains its high-to-low, overlap-safe copy order.
It names and publishes the method and both operand destinations after the FR2
companion slots and continuation have been installed. The direct string path
saves the result's stack offset before `lj_buf_str`, restores the slot after a
possible stack relocation, writes the new string, and publishes that root
before an optional GC step.

`lj_meta_call` continues shifting arguments high-to-low, so every source root
remains present until its replacement is visible. Each shifted destination is
published immediately; the FR2 copy of the original callable is published
next, and the method overwrite is published last. No source slot is cleared
during the transition, so intermediate scans can see duplicates but cannot
lose an operand root.

## Native validation

A clean native assert bootstrap with `LUAJIT_MT_ARM64_BOOTSTRAP`,
`LUAJIT_DISABLE_JIT`, and `LUA_USE_ASSERT` passed:

- the TG dispatch and deterministic root-publication source/object contracts;
- the focused root-publication runtime fixture;
- all 387 vendored stock tests;
- threading API, hook redispatch, coroutine handoff/dead-resume, and 320 FFI
  callback rounds;
- the metamethod runtime fixture under GC-worker pressure (`baseline=3`),
  covering arithmetic, ordering/equality, concatenation, callable objects,
  protected metatable semantics, and FFI `__eq`;
- a separate 1,000-round arithmetic/concat/callable stress with periodic full
  collections, including plain-string concatenation.

The current metatable contract passes its C-frame publication checks and then
stops at the intentionally missing rooted ARM64 `getmetatable` route. That red
boundary prevents this checkpoint from being mistaken for completed Stage 2.

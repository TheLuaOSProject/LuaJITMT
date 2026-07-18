# XSAVE-consuming generic FFI frame lifecycle (2026-07-18)

## Status and scope

This change adds the owner-only production helpers which turn the three
`IR_XSAVE` staging words into a pinned, remotely coherent
`LJFFINativeFrame`, enter native state, and symmetrically leave/pop/unpin it.
The helpers are still dormant: no IR call descriptor or recorder invokes them,
the unconditional generic `crec_call()` blacklist remains, and trace-flush
remote acknowledgement still uses the conservative `jit_base` veto.

There is one declaration-independent entry/leave pair.  It has no C-signature
shape enum, matcher, generated wrapper family, or ABI-specific argument code.
The future generic x64 `IR_CALLXS` seam can use the same pair for every ABI
shape which its existing classifier accepts.

No `plan/` file was edited.

## Entry preflight

`lj_ffi_native_trace_enter(L, T, func)` saves errno/LastError before any other
work.
It returns zero, requesting a pre-call side exit, without consuming XSAVE or
changing mirrors, frames, native depth or pins when any ordinary admission
condition fails.

Frame depth and native depth are checked before dereferencing the raw generated
trace constant.  Any active frame (including a full sixteen-frame stack)
therefore rejects even a deliberately poisoned trace pointer without touching
it.  An odd generation or out-of-range depth is single-owner structural
corruption and fail-stops instead of leaving a permanently ambiguous
publication.

This first owner helper admits only an outermost transition: both frame depth
and `in_native` must be zero.  A nested native leave decrements depth without
polling, so it cannot provide the consumed-poll park certificate while its
frame is popped and unpinned.  Callback/nested calls remain interpreted until
the later suspended-frame protocol makes the outer frame stable and restores
an outermost polling boundary.

XSAVE geometry is validated with integer byte arithmetic before pointer
construction:

- `L` is the TG's current carrier and has the exact owner/global identity;
- stack, maxstack, XSAVE root and current `jit_base` are present and aligned;
- root and JIT base are beyond the stack prefix and inside maxstack;
- `nslots >= 1 + LJ_FR2`;
- `baseslot <= nslots - 1 - LJ_FR2`;
- the complete staged extent fits before maxstack;
- `root <= base <= top` and `jit_base <= top`.

The persisted exclusive top is
`root + nslots - 1 - LJ_FR2`.  Root, base, top and JIT base are stored as byte
offsets from the current stack, so later stack relocation can resolve the same
logical values under a callback-suspension certificate.

## Exact trace admission

The executing trace's already-published `jit_base` is the independent lifetime
proof required before inspecting its exact `GCtrace *`.  Entry then uses a
nonwaiting GC2 SMR try-read, verifies the current public slot, and atomically
acquires one native trace pin.  It rechecks exact slot identity and nonzero pin
before dropping SMR.  Retirement may close admission after the increment, but
the pin then keeps the body and its public slot reservation resident.

Temporary SMR-writer contention or lost trace admission returns zero.  If the
post-pin recheck fails, entry releases the pin before returning and leaves all
owner state untouched.

## Publication and native ordering

After all fallible work, entry constructs a complete local frame containing
the exact body/slot, carrier, function, four offsets, prior callback mirrors,
entry STOPREQ state and the TG's acknowledged handshake epoch.

It then performs only nonthrowing owner operations:

1. push the frame through the existing odd/even publication protocol;
2. publish the resulting exact even frame;
3. clear all three XSAVE staging words;
4. install callback discovery mirrors and the entry STOPREQ snapshot;
5. increment native depth last;
6. restore the caller's errno/LastError pair.

A remote root acknowledgement can therefore never observe native state before
the exact synchronized frame and pin exist.  Every failure before the push
leaves staging available to the generated pre-call exit.

## Ordinary leave

`lj_ffi_native_trace_leave(L)` saves the immediate foreign errno/LastError pair
first.
It copies and validates the owner-published top frame, then calls
`lj_native_leave(L)` without changing any frame word or pin.  If a remote
leader consumed the request, the owner remains at that call until the exact
GC2 scan and leader boundary have finished.

After native leave returns, it:

1. detects a callback-slot change and any change to the TG acknowledgement
   epoch;
2. restores the surrounding callback slot, function mirror and sticky STOPREQ
   mirror;
3. pops the frame, publishing a stable even lower-depth stack;
4. releases the exact trace pin only after no stable frame names it;
5. restores the foreign error pair;
6. performs fresh STOPREQ handling with the frame's entry snapshot.

STOPREQ can throw only after the frame and pin are gone.  A normal return
preserves the foreign error pair again after the check.

The returned action word uses `LJ_FFI_NATIVE_LEAVE_FORCE_EXIT` in the reserved
high bit when a callback or remotely consumed handshake was observed.  Other
safepoint action bits pass through unchanged.  This dormant slice always
pops/unpins before returning; the next tranche must transfer a forced frame and
pin to trace-exit cleanup before allowing remote FLUSHJ retirement.  That
handoff is specified in `notes/ffi-pinned-postcall-exit-design-2026-07-18.md`.

## Deterministic evidence

The native-frame fixture proves that capacity rejection precedes raw trace
access and preserves errno plus every frame/mirror/native/XSAVE word.

The x64 XSAVE fixture uses a real finalized trace and real generated XSAVE
geometry.  It proves:

- malformed geometry has no side effect;
- exact slot admission increments one trace pin;
- the published offsets match the staged root/base/exclusive-top/JIT-base;
- XSAVE is consumed only after the even frame exists;
- callback mirrors and native depth are installed and restored;
- ordinary leave produces a stable empty frame stack and zero leaked pins;
- callback observation returns the force-exit bit;
- entry and the simulated foreign return each preserve errno/LastError.

Both focused gates compile their fixtures with assertions and `-Werror`.
Default, no-JIT and no-FFI runtime builds also compile.  The APIs are excluded
from no-JIT builds, where neither XSAVE nor generic `CALLXS` can execute.

## Remaining activation work

This slice does not yet provide:

- an authentic generated call to entry/leave or a real foreign `CALLXS` between
  them;
- retained `POSTCALL` frame/pin cleanup after a forced guard exit;
- callback suspension, callback blacklisting, or callback error unwind;
- result-register preservation across the leave helper;
- remote trace-flush admission for exact pinned frames;
- removal of the generic recorder blacklist.

Those boundaries remain conservative, so the dormant helpers cannot make an
existing Lua/FFI program take a new runtime path.

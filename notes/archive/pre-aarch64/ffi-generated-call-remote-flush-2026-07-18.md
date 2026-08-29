# Generated FFI remote trace-flush checkpoint (2026-07-18)

## Scope

This checkpoint lets a trace-flush handshake acknowledge a peer which is
blocked inside an authentic generated x64 `CALLXS`. The foreign function does
not have to return before `jit.flush()` completes. The path remains behind the
test-only generic-CALLXS activation gate; this change proves the remote
lifecycle needed before the production recorder gate can open. No `plan/` file
is changed.

Ordinary trace-entry and trace-exit `jit_base` windows are still conservative
vetoes. Only the generated FFI frame stack supplies the stronger certificate
described here.

## Two-stage remote admission

Before consuming a peer request, the leader accepts the exception only when a
stable even frame snapshot proves all of the following:

- native depth is exactly one;
- every lower callback continuation is `SUSPENDED` and the top frame is
  `ACTIVE`;
- every frame names the current Lua state and has aligned in-stack geometry;
- the top saved JIT base reconstructs the TG's published `jit_base`;
- positive trace vmstate and current FFI function mirror match the top frame;
- native depth and frame sequence remain unchanged after the snapshot.

Admission captures an immutable `trace_cert_required` obligation. It is not
recomputed from `jit_base` after request consumption. This matters when a
foreign function begins a Lua callback: `ACTIVE -> SUSPENDED` clears
`jit_base` while native depth is briefly still one. If that transition races
admission, post-consumption certification fails and the original request is
requeued rather than acknowledged without its promised proof.

After consuming the request and while poll remains held, the leader revalidates
the stable frame stack under a try-only SMR read lease. For every frame it first
loads the current `TraceVec` slot, then compares the raw frame pointer, and only
then reads the trace body's native-pin count. This order makes a malformed or
poisoned frame pointer fail without dereference. Recursive local frames naming
the same trace require the corresponding local pin multiplicity. The final
poll, request mask, and frame sequence must still describe the consumed stable
snapshot.

## Root scan, retirement, and owner close

If the action includes owner roots, the exact parked-native scanner runs before
the epoch is claimed. A failed SMR lease, frame validation, trace-graph mark, or
stack scan requeues the request without decrementing the existing pending slot.
A successful pre-scan is not repeated when the actions are applied.

The consumed poll freezes frame payload, stack storage, and pin ownership. If
the foreign call returns concurrently, native leave first closes native depth
and then waits on that poll; the exact `ACTIVE` frame cannot use the ordinary
trace-exit deadlock bypass. During final trace quiescence, a peer is exempted
only when its request is consumed, its acknowledgement is for the current
epoch, and the complete TraceVec/pin certificate still passes.

Trace retirement may close the slot while the exact native pins preserve the
body and mcode. When the leader clears poll, native leave observes the changed
handshake epoch, converts the frame to `POSTCALL`, and takes the existing
non-side-linkable caller snapshot exit. Central exit cleanup releases the pin.
The foreign call is neither replayed nor duplicated.

## Evidence

`t-ffi-callxs-remote-flush.lua` records the authentic generic scalar seam in a
worker, verifies both `XSAVE` and `CALLXS`, and blocks the eighth generated call
inside a shared C fixture. The main thread requires `jit.flush()` to return
before releasing the function, then checks the exact result, one native entry,
and one post-release side effect. It passes together with the authentic scalar,
forced-POSTCALL, callback normal/nested/error, safepoint, and XSAVE fixtures.

Feature-build coverage includes default, JIT-disabled, FFI-disabled, and
JIT+FFI-disabled GCC and Clang builds. The callback integration now guards its
generated-frame helpers with `LJ_HASJIT`, so the FFI-enabled/JIT-disabled
configuration has no unresolved helper calls.

## Remaining production boundary

Remote trace flush is no longer a blocker for the admitted scalar lifecycle,
but it does not open the default generic recorder by itself. Remaining work
includes nonallocating/pre-rooted handoff for the boxed and branching result
classes, exact additional Lua return-frame shapes, descriptor-driven aggregate
ABI results, broader callback/carrier stress, and removal of the test-only
activation gate. Platform ABI validation follows the Linux tranche.

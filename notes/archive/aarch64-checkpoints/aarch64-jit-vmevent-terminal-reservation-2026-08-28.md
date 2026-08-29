# AArch64 JIT omitted-terminal VM-event reservation (2026-08-28)

## Status

The bounded VM-event substrate needed by the first ARM64 side-child publisher
is implemented and tested. It is not yet wired into side publication, and the
production ARM64 side-recorder gate remains closed.

## Why an absence snapshot is not enough

`lj_vmevent_prepare_try()` can prove that the TRACE handler was absent across
one exact attachment-clock generation. A second stable snapshot immediately
before `ASM -> PUBLISH` still does not exclude a later `jit.attach()` writer.
That writer could install a handler before the runnable child edge and would be
entitled to observe the terminal TRACE event even though START had no handler.

The terminal omission therefore needs an ownership interval, not only a read.

## Reservation

`lj_vmevent_absence_reserve()` first validates the prepared ABSENT identity,
then reuses the authoritative event attachment writer lane. It must claim the
exact prepared even sequence and the immediately following generation. A
successful reservation leaves the lane odd, so a standard `jit.attach()` or
detach cannot commit the same event while the publisher owns the reservation.

There is no cancellation terminal for an odd lane. Every successful reserve is
paired with `lj_vmevent_absence_release()`, which uses the existing writer
publication operation. This advances the generation and conservatively sets
the VM-event cache invalidation mask even when the protected interval made no
registry change. A claim that discovers a newer generation is likewise
published before the stale reservation attempt refuses.

For the first-side transaction, the required lifetime is:

1. prepare and capture an ABSENT TRACE result while the operation is abortable;
2. reserve the exact TRACE lane at the final pre-seal boundary;
3. hold the odd lane through `ASM -> PUBLISH`, child/GC/topology publication,
   and the final authenticated parent-exit CAS;
4. release at the point which replaces ordinary STOP delivery; and
5. only then publish terminal recorder state and release the JIT token.

If any pre-seal step fails after reservation, release the lane before raising
the ordinary retry error. No event check, callback, wait, or fallible operation
is legal after `ASM -> PUBLISH` and before the runnable edge.

Compile-time `LUAJIT_DISABLE_VMEVENT` uses a checked no-op reservation because
there is no handler registry or attachment writer. A VM-event-enabled no-JIT
build has no clocked exclusion primitive and is deliberately refused by this
API; the ARM64 JIT publisher cannot exist in that profile.

This reservation closes successful no-handler publication against public
`jit.attach()`/detach. It does not implement the future durable TRACE-stream
cutover: an ordinary abort which loses to an already odd attachment transition
still has the legacy best-effort delivery behavior. Direct mutation of the
private `_VMEVENTS` registry table also remains intentionally unclocked; normal
Lua code cannot independently publish the internal dispatch-cache mask.

## Focused proof

`tests/t-vmevent-prepare-clocked.c` now checks:

- canonical INITIAL and PUBLISHED absence revalidation;
- rejection of wrong argument base, slot, attachment state, clock fields, and
  a set TRACE cache bit;
- rejection of odd, changed, and corrupt live clock shapes;
- a deterministic writer win between revalidation and claim, including the
  mandatory successor-generation publication and even-lane restoration;
- exact odd-lane reservation and a competing writer's bounded BUSY result;
- release to the next even sequence and generation with cache invalidation;
- stale prepared identity rejection after release; and
- the canonical disabled-VM-event no-op reserve/release path.

The focused fixture passes on native macOS arm64 in the experimental JIT and
experimental JIT plus `LUAJIT_DISABLE_VMEVENT` profiles.

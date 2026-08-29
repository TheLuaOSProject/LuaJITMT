# GC2 markword and activation primitive checkpoint (2026-07-11)

This checkpoint deliberately adds the final low-level primitives without yet
changing `GCArena` geometry or runtime GC behavior.  Adding a dormant second
liveness plane would leave two competing authorities and silently move arena
cell offsets; the first behavioral migration must instead convert allocation,
marking, VM/JIT fast paths, sweep, restore, and verification together.

## Epoch markwords

`LJArenaMarkWord` is one aligned CX16 pair containing `{bits, epoch}`.  A first
mark in a newer epoch resets old bits and sets the new bit in the same CAS.
Same-epoch markers merge through exact retries.  Older-epoch set/clear requests
are rejected, so a delayed worker cannot move a word backwards or erase newer
liveness.  Clears are exact-epoch, bit-local operations.

Readers use an acquire epoch/bits/epoch snapshot rather than a 16-byte atomic
load.  This preserves the inline, lock-free x86-64 contract and avoids compiler
runtime fallbacks.  Writers use the existing inline `cmpxchg16b` primitive.
Overlapping aligned 64-bit subloads and CX16 are an explicit target/compiler
contract, not a portable C11 claim.  GCC, Clang, MinGW, and Darwin artifacts
must continue to disassemble to inline CX16 and carry no libatomic import.

The eventual arena layout will use separate epoch markwords for authoritative
`live` and helpable `grey` work.  The structural bitmap currently called
`mark` must become `freehead`; it cannot remain a second liveness authority.

## Typed activation token

The earlier 64-bit `{mark_epoch, active}` direction is insufficient.  Minor
collections can reuse a mark epoch, so a complete IDLE -> MARK -> IDLE cycle can
look unchanged to a delayed barrier.  The new token is:

```
lo = mark_epoch
hi = transition_generation << 3 | typed_state
```

Every semantic phase transition increments `transition_generation`, including
transitions that reuse the same mark epoch.  States distinguish IDLE, PREP,
MARK, WEAK, SWEEP_OPEN, SWEEP_CLOSING, SWEEP_COMMIT, and sticky NO_RECLAIM.
The counter never wraps: saturation must enter conservative no-reclaim mode
while allowing mutators to continue.

Only IDLE admission into PREP or MARK may select a newer mark epoch.  All
within-cycle, abort, reopen, close, and commit edges retain the exact epoch.
Active states reject epoch zero.  MARK must pass through WEAK before opening
sweep, even when weak/finalizer processing is an empty phase.

This is an intentional divergence from the earlier 64-bit sketch because it
removes phase/cycle ABA and provides a typed sweep publication/commit cut.

## Tested properties

The standalone fixture combines bounded sequential-consistency schedule models
with real atomic/CX16 stress.  It covers:

- stale epoch E versus E+1 publication;
- a delayed E CAS losing after an E+1 CAS;
- unrelated-bit preservation during clear;
- concurrent same-word marking and set/clear retries;
- mark-before-block allocation publication under every bounded interleaving;
- sample/store/recheck across IDLE -> MARK -> IDLE, including minor-cycle ABA;
- exact transition CAS loss and monotonic mark epochs;
- legal typed state edges, generation saturation into absorbing NO_RECLAIM,
  and rejection of transitions out of that state; and
- concurrent stable snapshots while the token changes repeatedly.

The next behavioral checkpoint must add persistent helpable write-barrier
participants/descriptors and epoch-grey work before removing the current
phase counters or sweep obstruction mechanisms.  A descriptor is published
before its heap store; sweep admission and close/commit are exact generation
operations.  No current obstruction mechanism is relaxed by this primitive
checkpoint alone.

Validation for this checkpoint included native GCC and Clang, MinGW/UCRT under
Wine, and osxcross Clang under Darling.  Every target emitted inline
`cmpxchg16b`; none imported an atomic runtime.

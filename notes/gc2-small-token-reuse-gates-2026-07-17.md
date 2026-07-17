# GC2 small-token reuse gates and incarnation reset (2026-07-17)

## Scope

This tranche couples the dormant exact table-rescan token to small-arena
physical lifetime. It does not enable the descriptor/token producer yet. The
producer remains disabled until the ACTIVE/PINNED descriptor gate, huge-object
coupling, bounded enumeration, and counter-wrap policy are complete.

The key distinction is:

- `FREE` is logical allocation lifetime and may coexist with a non-`NONE`
  table token.
- A non-`NONE` token is a physical body/reuse owner. It prevents destructive
  body access, free-run publication/coalescing, reuse, and mapping teardown
  until exact completion returns it to `NONE`.

An allocation attempt that observes a token after claiming an unpublished
`CONSTRUCT`/`MUTATING` lane changes no body or structural bit. It therefore
rolls the exact claim back to the prior `FREE+token` state. The token remains
the reuse veto; retaining the construction lane would have no completion path
and would leak the cell.

## New-incarnation rule

Every fresh traversable small allocation now follows this order:

1. Observe token state `NONE`.
2. Claim `FREE` into a non-readable `CONSTRUCT` or `MUTATING` lane.
3. Recheck token state `NONE`.
4. Release-store zero to `LJGC2TabStamp.state` only.
5. Initialize the body and birth mark.
6. Publish READY/block/root identity.

`token.control` is never reset. Its generation is persistent side identity and
survives free/reuse. Clearing `state` removes an inherited scan-cycle/dirty
proof, including the latent cycle-wrap ABA where a new table could otherwise
look already scanned in a later cycle with the same 32-bit cycle number.

The C allocator, exact-bin reuse, private bump allocator, specialized typed
single/pair reservation, and root-construction paths share this rule. The x64
interpreter empty-`TNEW` fast path and the JIT x64 specialized `FNEW1NUM`
two-object bump path require equivalent generated-code checks because they
bypass the C allocator helper.

## Physical veto coverage

Small-token ownership now participates in:

- destructor and mutation claims plus their final validation;
- scalar/batched/pre-grace and exclusive GC destructor paths;
- late/quarantine terminal classification and the whole-word terminal
  certificate;
- complete-range ownership preflight, coalescing, free-run publication and
  intrusive free-run body validation;
- private bump-tail preparation and retained rediscovery boundaries;
- free-run enumeration and live-cell estimates;
- full-arena registry deletion/unmap checks already introduced with the token
  sidecar.

Cold bitmap-word summaries avoid rescanning all 64 token entries for every
candidate within the same word. Exact hot allocation checks remain one
sidecar/token load before the lifetime CAS plus the required post-claim check.

`lj_arena_lifetime_clear_terminal()` now reports
`LJ_ARENA_LIFETIME_CLEAR_BLOCKED` without changing the lane if a token proves
that its quiescent-cleanup precondition is false.

## Generated-code fallbacks

The interpreter empty-`TNEW` path checks the sidecar/token before consuming its
private cursor. A post-claim crossover rolls root `LINKING->NONE` and lifetime
`CONSTRUCT->FREE` exactly, then uses the ordinary helper. On success it clears
only the scan-state word before the first table-body byte and before
READY/block publication.

The specialized JIT `FNEW1NUM` path applies the same rule to both the closure
and closed-upvalue starts. Its common same-word pair uses one packed lifetime
transaction; split words retain exact per-lane rollback. Any packed-state
mismatch remains a terminal invariant failure rather than authorizing body
access.

## Deterministic coverage

Focused fixtures seed nonzero scan state and nonzero-generation `NONE` tokens,
then verify:

- generic bump and exact-bin reuse clear `state` and preserve
  `token.control`;
- typed pair reservation clears both states and preserves both generations;
- an interior `PENDING` token splits free-run enumeration and vetoes range
  coalescing without changing arena metadata;
- terminal-word and terminal-lifetime shortcuts refuse token-owned cells;
- an injected token on a typed destructor body prevents pre-grace body access
  while unrelated objects still reclaim, then allows progress after exact
  completion;
- x64 empty-`TNEW` uses the C helper without consuming the protected bump cell
  for `PENDING`, and resets/preserves the side words on inline success;
- JIT `FNEW1NUM` provides the analogous two-cell reset and veto behavior.

## Remaining blockers before producer enablement

1. Couple the global helpable descriptor to final destruction and unmap:
   matching `ACTIVE` must veto its exact allocation, while `PINNED`/malformed
   must conservatively veto every traversable allocation/mapping. Ordinary
   allocation can stay off this path because a lawful ACTIVE publisher holds
   reciprocal readable-lifetime admission; the destroyer must observe either
   that admission or the descriptor before producing reusable `FREE` storage.
   On the supported x86-64/GCC/Clang contract, an aligned acquire load of the
   descriptor low word is coherent with CX16 and avoids a locked snapshot;
   this mixed-width machine contract is stronger than portable ISO C and must
   be stated explicitly at the helper.
2. Apply the same token/descriptor protocol to all huge-table free, deferred
   free, destructor, realloc, transfer, delete, fini, and unmap paths.
3. Add bounded small/huge token enumeration and the specialized exact-token
   admission needed to scan logically free/deferred bodies safely.
4. Resolve 32-bit scan-cycle and dirty-epoch wrap without ABA.
5. Replace the eager 64 KiB sidecar-per-traversable-arena storage policy if a
   lower-overhead scheme can preserve allocation-free lookup and helpability.
6. Post-claim VM/JIT fallback consumes its private bump cursor before exact
   rollback. Reciprocal admission should make that crossover unreachable;
   rewinding or retaining a separate retry locator would improve corruption
   containment. Remote-free chains similarly rely on the rule that intrusive
   FREE nodes cannot acquire a later token before `next` is read.

These are correctness boundaries, not optional performance follow-ups. The
current checkpoint is safe because no runtime path publishes the new helpable
descriptor/token protocol yet.

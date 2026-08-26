# Apple ARM64 rooted protected-metatable lookup (2026-08-26)

## Claim boundary

The ARM64 base-library `getmetatable` fast function now resolves both the raw
metatable and its `__metatable` protection value from one admitted metatable
generation. The ARM64 `setmetatable` fast function now unconditionally enters
the existing C implementation, which owns type checks, protected-metatable
errors, replacement/clear semantics, release publication, and the GC edge
barrier.

This checkpoint covers the protected base-library operation only. The raw C
API path used by `lua_getmetatable`/`debug.getmetatable` still needs equivalent
exact source/metatable admission for concurrent mutation. Distinct
table/userdata equality, comparison rooting, and `ISNEXT` bytecode publication
remain open before Stage 2 can close.

## One-generation lookup protocol

`lj_meta_getmt_protected_rooted(L, objroot, outroot)` receives authoritative
`TValue` roots rather than a stripped object pointer. It records stack-relative
source/output offsets so a retry-capable wait can relocate the stack without
leaving stale addresses.

Each attempt opens source SMR before acquire-loading the receiver and admits
its exact `TValue`. The receiver lease authorizes the mutable metatable-edge
load; the same SMR interval stays open until that exact tagged metatable has
its own lease. This bridge prevents a replacement/reclaim/same-address
successor from being mistaken for the captured generation.

With receiver and metatable leases live, a second SMR interval performs the
bounded, non-waiting `MM_metatable` table lookup. A collectable protection
value acquires its own exact lease before vector SMR closes. The chosen result
is then release-copied to the rebased output and stack/root-published while its
result or metatable lease remains live. Stable absence, nil, or a stale
protection value exposes the already captured raw metatable; no second edge
read is allowed to mix generations.

Every retry closes SMR and releases all acquired leases before
`lj_tab_wait_l`. The output slot is deliberately not overwritten until the
attempt can no longer wait, because `BASE-16` is still the active fast-function
callable root during a retry.

## ARM fast-function routing

The ARM VM saves the real frame PC and `L->base`, passes `L`, the argument slot
at `BASE`, and the result slot at `BASE-16`, then reloads relocated `BASE` and
the frame PC. It enters `fff_res1`, because the helper has already written and
published the result; treating the returned pointer as a raw `TValue` would be
an ABI and lifetime error.

All object types use this route. Keeping a primitive or table inline subset
would retain the same naked mutable-edge lifetime hole. `setmetatable` has no
safe inline subset under replacement, clear, and protection races, so its first
operation is now the C fallback branch.

## Native validation

The final source passed:

- a clean native ARM64 assert bootstrap with JIT disabled;
- the complete bootstrap gate: TG/root source-object contracts, all 387 stock
  tests, threading/hook/coroutine checks, and 320 FFI callback rounds;
- the metamethod source/object contract and runtime retention fixture under
  the normal archive (`baseline=3`, FFI equality covered);
- focused nil/raw/protected-table/protected-false/replace/clear semantics,
  retaining raw and protected collectable results across full GC;
- an ARM64 amalgamated assert build, proving assembler-visible helper linkage.

The internal metamethod C fixture cannot link `lj_assert_fail` from the
amalgamated static archive; it passed against the normal archive, while the
amalgamated interpreter itself built successfully. This is a test-fixture
export limitation, not an unresolved VM/helper symbol.

The existing deterministic metatable capture/lease race fixture exercises the
shared admission protocol through `lj_meta_lookuptv`, but it has not yet been
retargeted to the new protected helper. Dedicated receiver/metatable/protected-
result retry injection remains a validation follow-up rather than a claimed
proof in this checkpoint.

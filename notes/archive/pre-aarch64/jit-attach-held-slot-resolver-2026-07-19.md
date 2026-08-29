# Held keyed-slot resolver for `jit.attach`

Date: 2026-07-19

This b1.2.1 substrate adds the dormant address-only resolver which the prepared
keyed table-store transaction needs. It is not wired into `lib_jit.c`, the
attachment clock, VM-event reads, or callback delivery yet, so production
`jit.attach` behavior is unchanged. No file under `plan/` was edited.

## Interface and lifetime boundary

`lj_tab_keyed_slot_resolve_held()` performs one bounded, non-allocating paired
array/hash scan. Its caller already owns exact table and key leases and a GC2
SMR reader retaining both vector roots. The result is tri-state:

- `FOUND` includes an in-range array cell whose value is nil and an existing
  hash key whose value is nil;
- `ABSENT` means no structural slot exists, or that the exact slot contains a
  FORWARD marker proven to belong to a stable current separated-array/hash
  generation; and
- `RETRY` covers invalid operands, KEYLOCK/live-handoff FORWARD/finalizer
  claims, current colocated FORWARD, malformed target values, a retiring
  vector, or a failed paired-generation proof.

The output is initialized to zero and remains zero for every non-`FOUND`
result. On `FOUND`, the implementation integerizes the fresh slot pointer while
the caller's SMR/vector authority is still live. Only that opaque integer
crosses the API; no naked `TValue *` is returned or retained.

Malformed, stale unrelated weak keys in a collision chain are skipped without
entering `lj_obj_equal()`. They cannot be the exact leased/type-valid requested
key, and treating them as structural retry would let an already-dead unrelated
weak edge permanently livelock lookup. KEYLOCK, FORWARD and finalizer-claim
keys remain structural retry because they describe an in-progress publication
rather than a terminal unrelated edge.

`lj_tab_keyed_slot_resolve_rooted_try()` supplies the exact leases and one short
SMR interval from authoritative table/key TValue cells. It binds admission to
the exact universe, TG, physical actor, `lua_State *` owner word and current-L
association, then revalidates that identity and both raw root values before an
address can escape. It is one bounded try: it never waits, allocates, assists,
safepoints or throws, and it releases every lease/SMR scope before returning.
Like the table-store guard, it admits the exact main-state/main-TG physical
owner during terminal `lua_close` finalizers after `cur_L` has been cleared.
Shutdown never grants this exception to a secondary state or TG; this preserves
ordinary close-time `__gc` calls without turning process shutdown into general
publisher authority.

## Allocation-capable attach path

`lj_tab_keyed_slot_resolve_or_insert_rooted_l()` is the separate L-aware path
for attach. It saves stack offsets at entry, restores and writes back both
in/out root pointers on every attempt, and carries only those offsets across
waits or allocation. Callers must use the returned root pointers after every
normal return, including failure or `RETRY`.

On `ABSENT`, a small `void` wrapper invokes `(void)lj_tab_set(...)` solely to
ensure a structural key. The legacy naked pointer result is never assigned,
compared, cast, integerized or otherwise used. After insertion, a wholly fresh
rooted resolver attempt derives the candidate address. A structural retry
closes all resolver authority before `lj_tab_store_wait_l()`; STOPREQ and
allocation errors therefore throw with no resolver lease or SMR reader live.
After a bounded `RETRY`, the L-aware path reloads its owner-stable roots and
returns immediately for a plainly invalid non-table parent or nil, NaN, hidden
VM traversal cursor, or tag/header-invalid key. The prepared transaction mirrors
the traversal-cursor rejection. Such immutable inputs cannot be repaired by a
yield; only a structurally valid root pair may enter the generation retry wait.

The allocation-capable form accepts stack cells and fixed-address,
owner-stable enumerated root cells. Stack cells are offset-rebased. A non-stack
cell must remain an authoritative semantic root for the entire invocation; a
concurrently mutable external cell is not a valid input after the short
resolver leases have closed. This is the intended `jit.attach` calling shape
and avoids pretending that a C by-value snapshot remains a semantic root
across a throwing allocation.

Future attach wiring should use resolve-or-insert plus
`lj_tab_keyed_store_prepare_snapshot()` for replacement, and the existing-only
rooted try plus `lj_tab_keyed_store_prepare_exact(expected_handler, nil)` for
detach. It must never let allocation, waiting or attach-table mutation occur
while the external attachment clock is odd.

## Focused evidence

`m5_tab_keyed_slot_resolver` covers nil-valued array and hash slots as
`FOUND`; true `ABSENT`; fresh insert followed by exact fresh-address resolve;
numeric collision-chain insertion; detach-facing integral `lua_Number` and
string hash keys; nil/NaN/null-output refusal with zeroed failure addresses;
VM traversal-cursor refusal in both resolver and early transaction prepare;
held lease/SMR use; stable separated-array/hash FORWARD as ABSENT, but current
colocated or live-handoff FORWARD as RETRY; rooted insert-capable repair;
re-exposed retiring array and hash roots; exact root/unclaimed-state refusal;
no structural mutation/no no-L wait on existing-only resolution; and a forced
retry plus physical stack relocation which proves both in/out roots are
offset-rebased. The existing
prepared-transaction fixture remains the consumer contract test. A synthetic
terminal-main-owner case clears both `cur_L` and `tg_hint`, then proves the
close-finalizer exception can still insert and freshly resolve a slot, with
the SMR reader count restored and the TG root descriptor idle. A separate
reachable `newproxy` runs an actual `__gc` during `lua_close`, calls a C resolver
callback after shutdown (the VM may republish `cur_L` while dispatching the
callback), performs insert plus fresh resolve, and verifies the same cleanup
invariants before returning to teardown.

The GC2 beta policy for custom `lua_Alloc` is unchanged: custom allocators are
still temporarily ignored while the internal-arena lifetime protocol is being
completed. This resolver neither widens that exception nor assumes a
callback-owned allocator.

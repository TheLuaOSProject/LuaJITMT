# GC2 helpable table-rescan primitives (2026-07-17)

## Status

This records the first two implementation slices of
`gc2-table-rescan-helpable-token-design-2026-07-17.md`. The new
descriptor and token are deliberately dormant in this checkpoint: existing
`GCtab.gc2_rescan_state` and `table_rescan_pending` behavior remains the active
authority until the remaining lifetime vetoes, enumeration, and traversal
completion are all present. Persistent small/huge storage is now installed,
but no production path publishes a token yet.

No change is made to `plan/`.

## Exact global descriptor

`LJGC2TableDesc` is one 16-byte-aligned CX16 word:

- `lo == 0`: `IDLE`;
- `lo == 1`: absorbing `PINNED`;
- aligned `lo > 1`: `ACTIVE` exact table identity; and
- `hi`: non-wrapping publication generation.

An exact all-zero no-op `cmpxchg16b` supplies snapshots, matching the universe
incarnation authority. `IDLE(g) -> ACTIVE(table, g+1)` increments generation;
after durable token publication, an exact helper clears only
`ACTIVE(table, g) -> IDLE(g)`. Reuse of the same address cannot make an older
ticket valid. Malformed stored authority and generation saturation become
`PINNED`; ordinary descriptor collision remains `BUSY` so the caller can help
the observed identity.

The dormant descriptor is initialized in `lj_gc2_init()` and placed beside the
cold rescan state in `GC2State`, avoiding displacement of early/hot phase and
worker fields. Runtime callers added by later slices must translate `INVALID`
input and every `PINNED` result into global activation `NO_RECLAIM`.

## Persistent per-table token

`LJGC2TableToken` is one 64-bit word containing `generation << 2 | state`.
The only valid states are `NONE`, `PENDING`, and absorbing `PINNED`; there is no
owner-only `CLAIMED` state.

Every logical request changes generation, including refresh of an already
pending request:

```text
NONE(g)    -> PENDING(g+1)
PENDING(g) -> PENDING(g+1)
PENDING(g) -> NONE(g+1)       exact stable-scan completion
```

The scanner captures an exact PENDING ticket, then later completion can clear
only that generation. A refresh between scan proof and clear makes the older
clear fail. Generation exhaustion and malformed state pin instead of wrapping.
Small-arena token generations live in persistent side metadata and must
not be reset by table construction or cell reuse.

## Preallocated small and huge storage

`LJGC2TabStamp` combines the existing 64-bit dirty-epoch/scan-cycle word with
one independently atomic 64-bit token. Every traversable small arena allocates
its complete 4096-entry, 64-KiB sidecar while the arena is still private. A
sidecar allocation failure rolls the mapping back and rejects the arena; no
registry or allocator list can observe a traversable arena without storage.
The initialized side pointer is release-published, and later lookup is entirely
allocation-free.

Huge allocations use the same 16-byte stamp embedded at offset 104 of the
existing header tail. `sizeof(GCAhdr)` remains 128 bytes, so the huge payload
geometry and external allocation ABI are unchanged. The legacy scan-stamp
path now works for both small and huge internal-arena tables. Custom
`lua_Alloc` remains temporarily unsupported as already documented: a future
token publisher must translate a missing stamp into activation `NO_RECLAIM`,
never into a successful handoff.

Small-arena direct unmap and allocator-list teardown now require every sidecar
token to be `NONE` before the sidecar or mapping can be released. `PINNED`,
`PENDING`, and malformed tokens all veto teardown. A `NONE` token retains its
advanced generation across free/reuse of the corresponding cell.

This is not yet the full lifetime coupling needed to enable producers.
Small-cell destructive/reuse transitions and huge free/realloc/delete paths do
not yet perform reciprocal token arbitration. A newly claimed small-cell
incarnation must also reset only the dirty/scan proof word to an unscanned
value before body publication while preserving the token generation. Until
those checks, reset ordering, and bounded enumeration land, the
descriptor/token plane must remain dormant.

## Fail-closed details

Failed descriptor publish/help CAS operations inspect the exact value returned
by the locked instruction. An observed malformed value is pinned immediately,
not reported as ordinary contention. Token completion has the same rule.
`lj_gc2_table_token_capture_pending()` classifies a side token without reading
the table body; its caller must acquire exact allocation lifetime and recheck
the captured control word before traversal.

The primitive cannot enforce the higher-level locator-overlap ordering.
Production code may call descriptor finish only after token refresh, the sticky
scan hint, and a worker wake are release-published.

## Verification

`tests/t-gc2-markword-token.c` covers:

- descriptor lifecycle, collision, same-address ABA, stale helper tickets,
  malformed state, invalid input, and generation saturation;
- token NONE/PENDING refresh, exact completion, stale completion, concurrent
  scanners, malformed state, and generation saturation;
- the initial exact PENDING-ticket capture contract;
- 10,000 descriptor publication/help generations observed by four concurrent
  readers, including 9,998 delayed same-address tickets; and
- a deterministic helper-versus-malformed-authority race which pins exactly.

The arena fixtures additionally cover eager zero initialization, sidecar OOM
rollback, 128-byte huge-header layout, allocation-free small/huge lookup,
generation persistence across exact cell reuse, and both direct and
allocator-list terminal-unmap vetoes.

The standalone test passes strict GCC and Clang builds, repeated stress, GCC
TSan, GCC ASan+UBSan, and Clang UBSan. Both optimized artifacts contain inline
`cmpxchg16b` and have no `libatomic` or out-of-line `__atomic` import. Clang 19
ASan exposed and motivated the x86-64 CAS wrapper correction documented in
`clang19-asan-cx16-workaround-2026-07-17.md`; that configuration now
passes too.

The full `m2_arena_all` suite passes. Assertion/paranoia helper builds with
both GCC and Clang pass `t-gc2-traverse`, and focused GCC/Clang sanitizer arena
fixtures pass with leak detection. The real `global_State` descriptor starts
as exact `IDLE(0)`.

## Remaining migration

Before the new plane can become authoritative:

1. couple non-NONE descriptor/tokens to every free, reuse, realloc, quarantine,
   and unmap predicate, and reset only the scan-proof word before publishing a
   reused cell incarnation;
2. add bounded small/huge token enumeration and joined-world preflight;
3. switch publishers and stable traversal completion to descriptor/token
   handoff while retaining the legacy count as a conservative veto;
4. prevent 32-bit dirty/cycle scan-proof wrap from becoming an ABA; and
5. instrument pre-store root-operation descriptors before typed activation
   `COMMIT` is allowed to authorize reclamation.

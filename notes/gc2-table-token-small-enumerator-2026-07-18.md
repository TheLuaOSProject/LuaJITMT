# GC2 dormant small-table token enumerator (2026-07-18)

## Status and scope

This checkpoint adds an executable, test-helper-only consumer for persistent
small-arena table tokens. It remains dormant in production:

- no production mutator publishes through this request helper;
- no scheduler worker invokes the enumerator;
- weak/phase close does not treat its sticky hint as live work;
- legacy table `NEEDSCAN`/pending accounting has not been cut over; and
- huge-table tokens have no enumerator in this tranche.

The per-global dormant state is deliberately small-only: one 64-bit sticky
generation hint, a 32-bit registry slot, a 32-bit cell cursor, and seven
64-bit diagnostic counters (72 bytes). The shared retry work in this checkpoint
also adds one private 64-bit monotonic deferred-work epoch, for 80 bytes of new
fields before any surrounding compiler padding. Future huge-lane cursor fields
were removed rather than imposing an unused internal-layout cost. This changes
only the fork's private
`global_State` layout, not the public LuaJIT API/ABI.

## Small-lane safety order

Enumeration is physical and queue-independent. A turn snapshots one small
registry slot and advances `{slot, cell}` before attempting the candidate, so a
transient owner cannot starve later identities. A registry mapping-lifetime
lease is acquired before reading the arena header, sidecar pointer, token, GC
header, or table body. The scanner then:

1. accepts only exact `FULL`/`BIT_ONLY` rescue admissions;
2. captures the persistent sidecar token before allocation-body access;
3. rechecks the exact token ticket and allocation lifetime;
4. treats `FREE` as an irreversible body boundary and completes only the
   sidecar token, without reading the GC header or structural planes;
5. validates a live table under the retained body admission;
6. performs an exact table scan proof; and
7. completes only the captured `PENDING(D) -> NONE(D)` generation.

`PINNED`, `INVALID`, malformed side identity, or structural contradictions pin
reclamation fail-closed. Transient mapping, lifetime, SMR, snapshot, dirty
epoch, and token-refresh losses retain the token for a later pass.

The API is identity-budgeted and lock-free, not a whole-operation latency
bound. `budget` limits physical identities attempted. Once admitted, proving a
table is exact is necessarily O(table size). No scanner path waits for an
allocator, mapping writer, lifetime owner, weak allocator, or peer scanner.
`worker_active` serializes scanner ownership and phase/cycle is rechecked after
claim.

## HugeTab primitives

The physical-slot snapshot and token-lease helpers use one split validation or
one CX16 attempt per acquisition. A token lease is distinct from an ordinary
body reader. Only `LIVE` and `DEFERRED` return a non-NULL mapping pointer;
`MISSING`, `OVERFLOW`, `FREEING`, and `BUSY` grant no lease and leave it NULL.
`LIVE` may transfer its exact counted ownership into an ordinary reader without
a lookup/count gap. `DEFERRED` is header-only and can hand the final
`DEFER_FREE` release to the existing terminal owner.

The unused address-search token mode was removed. Future huge enumeration must
start from a stable TG-registry identity plus a physical HugeTab slot; it must
not rediscover an address through the generic reader path.

## Weak-allocation durability

Both legacy and exact table traversal now treat weak-vector overflow allocation
failure as a retry. Telemetry is not identity. Exact traversal leaves its table
token pending; legacy traversal retains exact COUNTED membership and republishes
a concrete traversal locator through SSB, grey, or recovery topology (with an
allocation-free fail-closed veto on structural publication failure). Weak
closure therefore cannot pass while the failed table remains authoritative.

The headless `reserved > capacity` bridge guard is covered directly, and the
legacy `capacity == 0` allocation-failure/requeue/close-veto path is covered
through the production traversal. The larger A/B matrix—A fails after a full
vector while B successfully publishes an overflow head before A replays—remains
future live-cutover evidence. A non-NULL head is not itself proof that every
failed producer is represented; current live safety also depends on the
independently checked table-pending and concrete-queue gates.

The settled weak overflow count now measures published identities rather than
failed allocation attempts: it is transiently a reservation count during a
producer's publication, and a cap-full reservation is rolled back if its raw
overflow node cannot be published. This prevents persistent OOM from amplifying
the next cycle's vector size. The phase owner excludes reset while a discovery
reservation is in flight, preserving the contiguous ready prefix.

Legacy retry outcomes advance the private monotonic deferred-work epoch only
after an exact token, concrete queue/recovery locator, or fail-closed veto is
durable. Collection, explicit stepping, fixpoint, weak/sweep transitions, and
worker-owned drains compare that epoch around a bounded quantum and yield
instead of consuming the same failed identity repeatedly. Background workers
apply a minimum 1 ms retry backoff; wake-sequence changes interrupt each futex
wait but are absorbed until the deadline, while stop still exits promptly.

## Focused evidence

The helper fixture covers:

- exact small-table completion and child marking without grey/SSB/legacy
  membership from the dormant request;
- the real resumable `{slot, cell}` cursor with repeated budget-one turns;
- FREE completion with a poisoned GC header and no payload-read counter change;
- dirty-before-proof and generation-refresh-after-proof races;
- peer-scanner exclusion through `worker_active`;
- exact weak-record OOM retaining one token while a later token progresses;
- stale-generation and IDLE requests remaining observational;
- registered huge-table request rejection with its embedded token unchanged;
- the headless overflow-reservation close guard; and
- cap-full repeated weak-node OOM keeping the published weak count bounded;
- legacy weak OOM retaining COUNTED work and blocking weak closure until
  storage is restored;
- large-budget collect/step calls yielding after a small durable retry quantum;
  and
- a recovery-owned weak-table retry returning promptly with its state folded
  back to `PENDING`, then completing after storage is restored.

## Remaining cutover work

Before production enablement:

1. implement huge enumeration through stable TG-registry nodes and physical
   slots, with outer registry/SMR admission; `SWEEP+DEFER` may complete only the
   embedded header token while LIVE remains pending for body proof;
2. integrate descriptor/token publication and scanner work into phase-close
   predicates, including `tg_registry_incomplete` fail-closed behavior;
3. prove topology/pass epochs and exact no-work acknowledgement rather than
   relying on the sticky diagnostic generation;
4. resolve dirty-epoch, cycle, and token-generation wrap protocols;
5. run the full capacity-full A/B weak-overflow allocation-failure matrix; and
6. remove legacy table-rescan accounting only after every producer, consumer,
   close gate, terminal-unmap path, and diagnostic has migrated.

Custom `lua_Alloc` remains outside this temporary b1.2-era GC2 tranche as
documented elsewhere; the dormant request rejects it rather than manufacturing
unconsumable token work.

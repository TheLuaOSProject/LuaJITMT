# Monotonic shared new-key publication

Date: 2026-07-25

This checkpoint replaces the ordinary active-MT `KEYLOCK` publication window
with an owner-only, irreversible real-key protocol on Linux/x86-64. It is a
researched deviation from the earlier plan to give every ordinary insertion a
persistent descriptor. The private/pre-MT path is unchanged.

This does **not** remove the need for persistent descriptors from FINREG or
resize. FINREG publishes a nonnil claim and ordered-finalizer side effects;
resize can hold the only value copy in owner-local state. Those operations
still need durable, helper-readable state before their peer-dependent waits can
be removed.

## Protocol

The shared insertion attempt keeps the requested key in the existing TG root
anchor and canonicalizes it once after the private path has failed.

For an empty home anchor:

1. reserve one freecount debit;
2. CAS the key directly from nil to the canonical key;
3. publish the weak-key and normal table-key barriers; and
4. return the value slot only if the hash generation is still current.

For a collision:

1. reserve one freecount debit;
2. choose a node whose key and value are nil and whose `next` is NULL;
3. CAS its key directly from nil to the canonical key and publish the key
   barriers;
4. scan the key's home chain for a duplicate and its exact current tail;
5. CAS that tail's `next` from NULL to the claimed node; and
6. revalidate the hash generation before returning its value slot.

Every shared structural chain publisher now uses the exact-tail CAS. The
FINREG test helper was changed from head-prepend to tail-append so an ordinary
inserter and FINREG cannot publish the same key through disjoint CAS locations.
Private and unpublished replacement vectors may retain their cheaper
single-owner head construction.

The local result of the real-key CAS is the only ownership certificate. A
loser must never infer ownership merely because it reloads the same canonical
key.

## Irreversible outcomes

Once the ordinary real-key CAS succeeds:

- the key is never reset to nil;
- the freecount debit is never returned;
- the node's `next` is never overwritten by its insertion owner;
- an unlinked collision generation loser leaves a nil-valued old-generation
  node, while a reachable empty-anchor claimant may be filled by a peer;
- a same-key loser leaves a nil-valued unlinked node; and
- a different-key tail-CAS loser retains the same claimed node and rescans
  instead of consuming another node.

A nil-valued claimant is semantically absent. Current iteration and GC scans
load or gate on the value side, while resize visits every physical slot and
copies only nonnil values. Thus resize ignores the claimant but independently
migrates any live node appended through the claimant's physical-anchor role.
`tab_rehash_hashcount()` still counts the claimant conservatively, so the debt
can temporarily over-size a replacement generation.

Stable links never target nil-key nodes. Under that existing invariant, the
candidate's nil/nil/NULL eligibility plus irreversible real-key claim gives it
no pre-existing inbound edge. Only its owning insertion can add that inbound
edge. Other insertions may append newer descendants through its physical hash
anchor, but cannot link an existing target-chain node into that fresh
component. The exact-tail link therefore preserves an acyclic next graph.

## Cost

The uncontended collision path has three locked RMWs:

1. freecount reservation;
2. real-key CAS; and
3. exact-tail CAS.

That matches the old `KEYLOCK` collision's three RMWs while removing its plain
candidate-next write and final canonical-key store. The rejected CAS-init/head
variant needed four RMWs and created more abandoned-node debt under unique-key
contention.

Seven interleaved, core-pinned best-of-five `tab_insert_newkey` runs compared
the exact pre-change commit `c832d41d` with this worktree. Median time moved
from 1030.78 ns/op to 1027.02 ns/op, about 0.36% faster and within measurement
noise. The container had no stock LuaJIT comparator, so this is evidence of no
regression against the fork baseline, not a stock-parity result.

## Model and audit evidence

The bounded model paused after reservation, key CAS, scan, tail CAS,
generation validation, store, delete, and resize steps. Its final exhaustive
runs covered same-key, different-key, and cross-physical-anchor schedules
without a cycle, duplicate live key, or lost completed store. Random
six-actor scheduling ran 50,000 cases without a reported safety or progress
failure. A Linux/x86-64 release/acquire litmus ran 3,000,000 iterations without
the forbidden both-old result.

Static source audit found no unguarded Linux/x64 active-MT production caller
which stores through a returned new-key slot without keyed generation
revalidation. The x64 VM and active-MT JIT use keyed helpers; remaining raw
stores are private construction/materialization paths. Legacy non-x64 VM
backends remain part of the deferred release-platform audit.

## Deterministic coverage

`m5_tab_newkey_monotonic` covers:

- a historical private same-bucket chain followed by shared exact-tail
  insertion without changing the private prefix;
- an empty-anchor publisher paused after its real-key CAS while a same-key
  writer completes, followed by resize and owner reroute;
- a collision publisher paused after its real-key CAS while a same-key writer
  completes on another node;
- a forced different-key tail-CAS loss, with the loser reusing its exact node
  and appending after the winner;
- FINREG winning the exact tail after an ordinary publisher's tail snapshot;
- a key hashing to a paused claimant's physical node and completing through
  the claimant's `next`;
- resize migrating that physical-anchor child while the claimant remains
  paused and nil-valued; and
- quiescent graph validation from every physical bucket: in-vector links,
  bounded termination, live-key reachability from the correct hash anchor, and
  matching physical-live and `next()` counts.

The final Linux evidence includes:

- 100/100 repeated focused deterministic runs;
- the focused FINREG, KEYLOCK, chain-order, resize-copy, structural-owner,
  colocated-resize, C API resize, and full table-resize stress cases;
- strict GCC and Clang helper/assert builds with `-Wall -Werror`, plus the
  fixture itself with `-Wall -Wextra -Werror`;
- Clang ASan: 20/20 focused runs, 20/20 FINREG runs, and a
  four-writer/two-GC-worker resize stress;
- Clang UBSan with the repository's documented emitter exclusions: the same
  focused repetitions and resize stress.

The broad `m5_concurrent_objects` aggregate was also attempted. It passed
through this new fixture, then stopped at the pre-existing
`m5_tab_forward_filter` timeout. The identical timeout reproduces at exact
baseline `c832d41d`: that fixture fabricates a stable current-generation
`FORWARD` without a publisher, while the bounded setter introduced by
`f37baada` classifies it as a generic retry forever. This is a separate b1.2.1
regression to repair; it is not counted as a passing aggregate.

`m9_newkey_barrier_scope` likewise currently exceeds its fixed grey-drain
threshold on both this worktree and `c832d41d`. Twenty paired process samples
gave the same counter ranges for current and baseline, so there is no measured
delta attributable to this protocol, but the gate is not reported as passing.

## Residual boundaries

- A paused FINREG owner can still expose `KEYLOCK`/`FINCLAIM` and make ordinary
  code retry or wait. FINREG still needs a persistent helpable descriptor.
- Resize still needs a persistent descriptor published before any source value
  becomes `FORWARD`; the current owner can otherwise hold the only payload in a
  C local.
- Same-key contention consumes nil-valued claimant capacity until resize
  compacts it, and each losing real-key claim runs the key barriers.
- A claimed node can acquire a physical-anchor suffix. Linking it can splice
  that suffix into another chain and amplify collision length beyond the
  pre-link `LJ_TAB_MAXCHAIN` check. Existing stable physical-anchor chains
  already share suffixes, but the one-CAS merge is an explicit adversarial
  performance/DoS risk to benchmark and constrain during the structural
  descriptor tranche.
- Exact-tail append no longer gives newly inserted colliding keys the previous
  head-insertion recency bias. The measurement above covers insertion, not
  collision-heavy lookup locality. That lookup benchmark and a comparison
  against an actual stock LuaJIT build remain release evidence.
- The deterministic fixture uses rooted string keys and its hook runs after
  both key barriers. Existing active-MT object/weak-key resize stress is
  indirect coverage; a collectable-key pause specifically between real-key CAS
  and the barriers remains focused-test debt.
- Portable cross-address ordering between hash retirement and tail publication
  remains part of the non-x64/release audit. Linux/x86-64 callers use keyed
  post-publication revalidation; no broader platform claim is made here.
- macOS and Windows runtime/fix work remains deferred until b1.2.1 is otherwise
  release-ready.

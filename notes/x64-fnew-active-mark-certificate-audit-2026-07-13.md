# x64 numeric FNEW active-MARK certificate audit

Date: 2026-07-13

Audit base: `c18c4a0e` (`Pack typed arena unmarked classification`)

## Status and verdict

The x86-64 traced one-local-numeric-upvalue `FNEW` path can safely remain
inline during GC2 MARK, but only after an exact current-cycle traversal
certificate has been published for both existing graph-bearing children of the
new closure:

- the exact child `GCproto *`; and
- the exact `GCtab *` environment snapshot stored into the new closure.

Restoring the old `mark_active && alloc_black` shortcut from `91c70acc` is not
an acceptable proof. The function and upvalue birth marks preserve those two
new allocation bodies for the current cycle, but do not by themselves prove
that the prototype or environment payloads have been traversed. The recommended
first implementation is a per-TG exact-pair cache seeded by the existing C
fallback. The first FNEW for a pair in a MARK cycle takes the C path and
publishes the certificate; later FNEWs for that exact pair and cycle use the
existing rootless typed inline constructor.

This note is an implementation audit. It does not modify `plan/` and does not
broaden the current inline allocator's sole-main-TG, no-worker, no-MT admission
predicate.

## Current implementation boundary

`asm_fnew1num_inline_x64()` in `src/lj_asm_x86.h` recognizes only the exact
one-local-numeric-upvalue helper shape. At this audit base it:

1. checks the private traversable bump window and exclusive allocator state;
2. claims rootless typed lifetime for one `LFUNC1` and one `CLOSED_UV`;
3. initializes the complete function and closed-upvalue bodies;
4. installs the allocation birth marks, destructor kinds, READY bits and block
   discovery boundaries;
5. commits the typed rootless lifetime; and
6. returns the function without publishing either object on the pending-root
   ownership stack.

The path currently branches to `lj_func_newL_gc1num_forjit()` whenever
`TG.mark_active != 0`. The comment at `src/lj_asm_x86.h:1547` states the exact
reason: birth marking does not publish proto/environment/upvalue traversal
work.

The C helper first calls `func_fnew_preserve_operands()`
(`src/lj_func.c:290`), which root-barriers the parent closure, parent prototype,
and child prototype before allocation can assist GC. Its bump constructor then
captures the parent environment, initializes the function and upvalue, installs
the birth marks and discovery metadata, and calls `func_pubfreshobjobj()` for
the child prototype, environment, and upvalue before committing typed lifetime
(`src/lj_func.c:705`). The marked-prototype path deliberately uses
`lj_gc2_barrier_marked_proto()` because an arena birth mark is not proof that a
prototype traversal was ever queued.

The fresh upvalue is a closed numeric cell. It has no GC payload edge in this
specialization, so `func_pubuv_payload()` has no work. The C constructor stores
the mutable BASE-slot link after typed commit. The current generated inline
constructor stores that BASE-slot link before its final typed commit; that is
safe only under its present sole-owner admission plus the live `jit_base` gate
proof. It must not be generalized to concurrent observation without either
retaining that proof or moving the store after publication of the complete
upvalue.

## Why the historical naked path is rejected

Commit `91c70acc` admitted active-black FNEW inline, cleared both `nextgc`
links, and skipped pending-root publication. Its focused test manually set
`mark_active=1` and `alloc_black=1`; the collector phase itself remained IDLE.
That test proved emitted branching and birth-mark layout, not a real GC2 MARK
fixpoint.

Commit `bc17bd6f` later introduced cooperative native execution during MARK.
The important change is the persistent root-snapshot certificate. Once
`gc2.mark_root_scanned` reaches state 1, `gc2_fixpoint_round()` intentionally
reuses it across bounded mutator turns. The source comment at
`src/lj_gc2.c:17686` is explicit: stack changes after the completed snapshot
are protected by barriers, and only concrete NEEDSCAN/JIT reopen work requests
another owner scan.

Therefore the following argument is invalid:

1. a JIT frame is active, so MARK cannot close;
2. create a birth-black closure without publishing its child traversal work;
3. clear `jit_base`; and
4. assume a later root snapshot will discover the closure's prototype and
   environment.

The completed snapshot may remain authoritative. A whole native turn can start
and finish between collector observations, and `jit_base` is a close veto, not
a post-snapshot mark-work publication.

Current snapshot restore and ordinary table/root stores do rescan an
already-marked escaping function. It may be possible to formulate a much
broader native-private-object theorem in which every possible FNEW result
either dies before quiescence or crosses such an escape barrier. That theorem
would have to cover every trace link, side exit, normal exit, error unwind,
same-trace call, stack restore, table/key/upvalue store, and future traced FFI
path. It is not the local invariant implemented by FNEW today and would be
fragile under later JIT work. This audit therefore does not use it to justify a
naked inline constructor.

Commit `0575ac4f` correctly removed ownership-spine publication only after
adding immutable arena destructor identity, and correctly restored the active
MARK C fallback. The destructor sidecar proves physical type and extent; it is
not a semantic root or a traversal certificate.

## Exact graph and barrier obligations

For this one-upvalue numeric shape, the new graph is:

```text
new GCfuncL
  -> exact child GCproto
  -> exact captured environment GCtab
  -> fresh GCupval

fresh GCupval
  -> numeric TValue             (no GC edge)

mutable owner BASE slot
  -> fresh GCupval
```

The function and upvalue arena birth marks preserve their allocation bodies for
the current cycle. The numeric upvalue needs no payload barrier. The BASE-slot
edge does not require another current-cycle child mark once the fresh upvalue
birth mark is installed, although its visibility must remain ordered before
native quiescence.

The child prototype and exact environment are different. Both are existing,
graph-bearing objects. The prototype traversal owns immutable chunk-name, KGC,
trace, and snapshot-PC reachability. The environment traversal owns its table
graph and its current table dirty generation. Both therefore need durable mark
or traversal work unless an exact same-cycle completion certificate already
proves that work.

The parent closure and parent prototype are not edges of the newly constructed
function. Their root barriers are still mandatory in the generic C helper
because an allocation/check/assist may run while those operands exist only in
native arguments. A pure inline region with no call, assist, or safepoint can
omit those two barriers after it has published exact child-prototype and
environment work: MARK cannot reclaim either operand while `jit_base` remains
live. Any design which calls a GC-capable helper before completing publication
must preserve the parent, parent prototype, and child prototype exactly as the
current C helper does.

If the specialization is later widened to capture a GC TValue, the exact
upvalue payload also needs a barrier. This audit does not authorize that
extension.

## Recommended representation

Add one owner-written, non-root certificate entry to `TGState`, conceptually:

```c
GCproto *fnew_cert_pt;       /* Pointer identity only; not a root. */
GCtab   *fnew_cert_env;      /* Pointer identity only; not a root. */
uint32_t fnew_cert_cycle;    /* Release-published validity word. */
```

The pointer fields must never be included in a root scan. Keeping them as roots
would retain stale prototypes/environments across future cycles. They are safe
as comparison-only stale pointers because the cycle word controls validity and
the pointers are never dereferenced on a cycle mismatch.

Reset `fnew_cert_cycle` to zero during the synchronous MARK activation
handshake, before `jit_mark_resume` and `jit_phase_gate` are opened. Newly
attached TGs start with a zero certificate. The reset removes 32-bit cycle-wrap
ABA as an acceptance path; cycle zero must always disable the certificate.

Certificate publication is:

1. publish durable traversal work for both exact objects;
2. store `fnew_cert_cycle = 0` if replacing a prior entry;
3. store the exact prototype and environment pointer identities; and
4. release-store the current nonzero cycle last.

The generated fast path acquire-loads the cycle first and accepts the entry
only if it equals the current MARK cycle, `jit_mark_resume` equals that same
cycle, and both exact pointers compare equal. A one-entry cache is sufficient
for the closure benchmark and minimizes TG state. Interleaved FNEW sites may
churn it safely; each miss simply republishes another exact pair. A small
set-associative cache is a later profile-directed option, not a correctness
requirement.

## Recommended C-seeded sequence

The lowest-risk implementation keeps the first exact pair in a cycle on the C
fallback and uses that construction to seed the cache.

### Generated active-MARK miss/hit

The generated template must retain all current allocator eligibility checks
and add these active-MARK predicates before any bump cursor or lifetime
mutation:

1. global phase is MARK;
2. `mark_active != 0` and `alloc_black != 0`;
3. the nonzero current cycle equals `jit_mark_resume` and the gate is open;
4. the current TG is still the admitted sole internal-arena owner; and
5. the exact cache cycle, child prototype, and captured environment match.

Load `parent->env` once for the comparison. On a miss, branch to the existing C
helper before claiming either typed lane. On a hit, the environment written to
the new function must be the exact value that passed the comparison. Do not
compare one load and later reload `parent->env`: a racy `setfenv` could make the
certificate cover one table while the new function points at another. Retain
the captured value in a register, or initialize from the certified cache entry
whose equality was just proved. The activation/gate protocol prevents a cache
reset while this trace remains active.

Outside active MARK, preserve the current inline path and avoid adding cache
traffic to the ordinary IDLE/SWEEP hot path.

### C construction and seeding

The first miss uses `lj_func_newL_gc1num_forjit()` unchanged for semantic
construction. Extend the successful active-black bump case as follows:

1. preserve the parent, parent prototype, and child prototype before any
   GC-capable operation, as today;
2. capture the exact parent environment once and use that same pointer in the
   function body;
3. initialize the complete function/upvalue pair and run the current proto,
   environment, upvalue, and payload barriers;
4. while the exact local `pt` and `env` values are still available, try to
   publish a two-object, no-drain traversal request;
5. only after that exact request is durable, publish the TG cache pointer fields
   and release-store the current cycle; and
6. finish the existing typed commit and BASE-slot publication.

The certificate publisher should have a status-returning internal API. It must
not infer success from the existing void barriers. A simple implementation is
an owner-local pair append to the active SSB:

1. revalidate MARK, cycle, `mark_active`, and `alloc_black`;
2. acquire-load `ssb_next` and `ssb_end` and require two complete free slots;
3. store raw object references for the exact child prototype and environment;
4. release-advance `ssb_next` once, exposing both initialized slots together;
5. publish the pointer cache and release-store its cycle; and
6. return success.

It must not allocate, drain, wait, assist, acquire the worker token, or publish
half of the pair. With fewer than two slots it returns failure without changing
the SSB cursor or cache. The ordinary C barriers still make that particular
closure safe; the next FNEW remains a cache miss and may retry after an ordinary
SSB flush. This makes certificate failure a performance fallback, not a liveness
or correctness failure.

The pair append may run after the current C barriers and before the typed pair
commit. Its entries point only at already-existing prototype/environment
objects, so they remain valid even though the fresh function is still private.
No worker wake is needed for a below-capacity active SSB: fixpoint emptiness
includes every live TG's active cursor, and the close handshake flushes that
concrete work.

## Why an SSB pair is a certificate

`gc2_ssb_mark_one()` treats a raw SSB object entry as an explicit traversal
request. For an already-marked graph-bearing object it still publishes worker
traversal; for a table it may suppress the duplicate only when the current table
scan stamp already proves the graph is covered. The request therefore handles
both white children and birth-marked-but-unscanned children.

`lj_gc2_ssb_empty()` includes published SSB nodes, grey/recovery work, the main
TG active cursor, the current TG active cursor, and every registered live TG
active cursor. A MARK fixpoint cannot close over the newly advanced cursor. A
flush may rotate the node and a worker may consume it, but either the request
remains represented as SSB/grey/recovery work or its traversal has completed.

For a cache hit in the same cycle, the earlier pair request is consequently in
one of three safe states:

- still visible in the active or published SSB;
- converted to visible grey/recovery work; or
- fully consumed, with the exact object marked/traversed for this cycle.

No physical reuse can occur between those states and a same-cycle MARK cache
hit. A phase abort makes `mark_active` false; a later activation resets the
certificate before reopening native entry.

The essential order is:

```text
pt/env slot stores
  -> release SSB cursor
  -> cache pointer stores
  -> release cache cycle
  -> fresh body/discovery/lifetime/result publication
  -> jit_base clear
```

The collector closes the gate and observes no active `jit_base` before final
fixpoint. It then acquire-observes the active SSB cursor. Thus it cannot both
miss the native execution and close before the traversal request is represented.

## Inline SSB miss alternative

Publishing the pair directly from generated x86-64 is also viable. It saves the
single C-constructed closure per exact pair/cycle, but is not the recommended
first change because the FNEW template is already large and has required
multiple mcode-limit splits.

An inline miss must:

1. perform all phase/cycle/allocator checks before either typed claim;
2. capture the exact environment once;
3. require two SSB slots before any irreversible mutation;
4. store prototype and environment, release-advance the cursor, then publish
   cache pointers and cycle;
5. initialize the function from that same captured/cached environment; and
6. keep every failure edge before lifetime claim on the existing C fallback.

The current sole-main-TG/no-worker/no-MT predicate makes the active SSB cursor
owner-private throughout the no-call template. A remote gate close may set a
poll, but it cannot apply a root/SSB boundary in the middle of active mcode; it
must wait for owner quiescence or the next sanctioned exit/poll. Slot stores
precede the cursor store, and the cursor precedes cache validity.

A small specialized C certificate helper called only on a generated cache miss
is a middle option. It must be no-drain and status-returning. Calling any helper
on every FNEW would surrender much of the call-boundary gain and complicate
register/argument preservation, so it is not recommended.

Publishing the fresh function itself to the SSB on every allocation would also
be correct after complete body publication, but it forces traversal of every
closure/upvalue pair and adds one queue item per allocation. The exact-pair
cache reduces this to two entries per distinct prototype/environment pair per
cycle.

## Rejected shortcuts

The following are not certificates:

- **Fresh function/upvalue mark bits.** They cover only the fresh bodies, not
  the existing child graphs.
- **A child arena mark bit.** An object can be birth-marked before any traversal
  request exists.
- **`LJ_GC_NEEDSCAN`.** `gc2_root_rescan_later()` explicitly documents it as a
  deduplication hint, not proof that an SSB entry remains visible. A stale bit
  can outlive consumed or abandoned queue work.
- **An active `jit_base`.** It delays close but does not request a scan after it
  clears.
- **The persistent root-snapshot state.** State 1 is deliberately reusable and
  is the reason a new barrier request is needed.
- **Parent closure identity.** `setfenv` can change the exact environment even
  when the parent pointer is unchanged.
- **`proto_gc2_scan_cycle` alone.** A matching nonzero cycle is a valid
  prototype-only completion proof because the stamp is stored after all proto
  children are handled, but it says nothing about the exact mutable
  environment. It is an optional later optimization, not a pair certificate.
- **Manual `mark_active`/`alloc_black` tests.** They do not model phase, gate,
  persistent snapshot, SSB, or transition behavior.

## Required deterministic coverage

### Real cooperative MARK

Extend the FNEW fixture with a real MARK case rather than only changing TG
mirrors:

1. warm and decode the FNEW trace in IDLE;
2. enter MARK through `lj_gc2_mark_begin()` and verify activation handshake,
   current cycle, resume generation, black allocation, and open gate;
3. establish a completed persistent root certificate, then reopen a bounded
   MARK native lease without resetting state 1;
4. run the traced FNEW loop;
5. assert that the first exact pair used the C helper, later loop allocations
   did not, and no pending ownership root was published;
6. prove the exact two SSB items or their grey descendants prevent MARK close
   until drained; and
7. complete MARK -> WEAK -> SWEEP -> IDLE and invoke the surviving closure.

The crucial scheduling case is a complete native turn which begins and exits
between collector observations while the old root state remains 1. The test
must not pass merely because another global root snapshot happens to rescan the
new closure.

### Exact prototype/environment semantics

Use a custom non-global environment containing a sentinel graph. Remove every
other reference to the environment, parent function, and parent prototype so
the escaped child closure is the only remaining semantic path. After the real
cycle, invoking the closure and querying its debug/prototype identity must still
work.

Change the parent environment between traced runs. Each new exact environment
must miss the cache once, become the environment actually stored in all later
closures, and survive collection after external references are dropped. Add a
test pause between environment capture/comparison and function initialization;
a test thread or hook can swap the parent's environment there. The closure must
use the exact certified pointer, never a later reload.

Exercise at least two child prototypes and repeated environment changes so a
one-entry cache churns without accepting the wrong pair.

### SSB and publication failures

Cover active SSB states with zero, one, and two remaining slots. Zero/one must
leave the cursor and cache untouched and use the C path before any inline typed
claim. Two must expose both initialized entries through one cursor advance.

Add pause points:

- after the first/second slot store but before cursor publication;
- after cursor publication but before cache-cycle publication;
- after cache publication but before fresh block discovery; and
- after block/lifetime publication but before `jit_base` clear.

At each point, a close attempt must either observe active JIT or visible SSB
work; it must never reach WEAK early. Force an SSB FLUSH rotation after cursor
publication and verify that the cache remains usable because the work moved to
the published/grey representation.

Inject a stale prototype/environment NEEDSCAN bit with no queue work and a zero
cache. It must not authorize inline execution. Abort a MARK after seeding the
cache, start another cycle, and verify the new activation resets it. Exercise
cycle zero/wrap test hooks so no stale pointer pair can compare valid by ABA.

### Generated-code and regression matrix

Extend the existing executable mcode decoder to verify:

- the active hit path no longer targets the generic helper;
- the miss target is reached before any lifetime claim;
- the exact environment comparison and the function environment store use one
  captured/certified value;
- SSB slot stores precede cursor publication, which precedes cache-cycle
  publication; and
- body, dtor kind, birth mark, READY, block, typed commit, and result order
  remain intact, including the cross-lifetime-word cold arm.

Run at minimum:

- `m6_jit_fnew_bump`;
- `m6_jit_gc2_readiness`, including the real MARK fixture;
- `m3_gc2_worker_scheduler`;
- `m3_gc2_paranoia` with stock JIT, no-JIT, and default restore;
- focused arena sweep/recovery tests;
- assertion, ASAN, and UBSAN builds where supported; and
- repeated standalone runs of pause/race fixtures.

The current `test_traced_immutable_numeric_inline()` manual-mirror section only
proves that active state reaches the forced C branch. Keep it as a decoder guard
if useful, but do not count it as the real MARK lifetime/barrier test.

## Expected performance value

The available 32K-sample `closures_upval` profile attributes whole-workload
samples to several functions used by the active-MARK C path:

| Symbol/operation | Samples |
| --- | ---: |
| `func_arena_set_alloc` | 4.48% |
| destructor-kind publication | 3.67% |
| `func_newL_gc1tv_bump` | 3.38% |
| lifetime reserve | 2.93% |
| typed pair commit | 2.51% |
| packed lifetime/block work | 2.03% |

These percentages are not additive savings. The inline path still performs most
of the same bitmap, lifetime, destructor and accounting operations in generated
code. The actual gain is the removed helper ABI boundary, repeated eligibility
and operand-preservation work, and repeated constructor-barrier dispatch after
the first exact pair.

The C-seeded cache pays one C construction and two SSB entries per distinct hot
pair per MARK cycle, so it should capture almost all of the steady-state active
loop benefit without enlarging the miss machinery. A reasonable expected
whole-workload improvement is single-digit to low-teens percent; the profile
places an optimistic upper bound below roughly 20 percent. Historical
`91c70acc` results likewise described helper-boundary removal rather than a
broad closure-throughput cure.

At the packed-classifier checkpoint, `closures_upval` is about 305--306 ns/op
versus roughly 37--38 ns/op for stock LuaJIT, approximately an 8.1x active-GC
gap. Active-MARK FNEW certification is worthwhile, but cannot by itself close
that release-performance gap. Measure clean pinned active/stopped pairs after
landing it and keep collector-cycle counts, retained bytes, helper calls, cache
misses, and SSB entries as co-equal debt/progress checks.

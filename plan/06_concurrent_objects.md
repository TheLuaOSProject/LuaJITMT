# 06. Concurrent Object Protocols

This document specifies every shared mutable runtime structure: tables
(§6.2–6.3), the string intern table (§6.5), upvalue cells (§6.4), thread
ownership (§6.7), and the long tail (§6.8). Executable reference for the
table protocol: `aux/nbtab_model.c` (compiles standalone, stress-tested;
port it rather than re-deriving).

## 6.1 Reserved internal value encodings

LJ_GC64 NaN-tagging leaves internal itypes free below the public ones
(lj_obj.h:260–284). Reserve two **never-Lua-visible** encodings:
```
LJ_TFORWARD  : itype pattern (~14u)  — "slot migrated; look in next gen"
LJ_TKEYLOCK  : key-claim sentinel    — "key being installed, retry"
```
Static-assert they collide with nothing (LJ_TNUMX is ~13u; pick ~14u and
shift LJ_TISNUM guard accordingly — verify `tvisnum` tests still hold:
they compare `itype < LJ_TISNUM`, so new tags must be < LJ_TNUMX numerically
…(~14u < ~13u), i.e. *larger* complement — confirm with a unit test that
prints all tag comparisons; the model file embeds the chosen constants).
These tags appear only inside Node.val / Node.key / array slots during
migration windows and are filtered by every read path below.

Current implementation note: the direct `~14u`/`~15u` itype-space approach was
rejected because it disturbed canonical NaN classification on x86-64. The
current x64 bridge represents `LJ_TFORWARD` and `LJ_TKEYLOCK` as raw
lightuserdata values in reserved segment 255 (`LJ_TFORWARD_BITS` and
`LJ_TKEYLOCK_BITS`). Public lightuserdata interning never allocates that
segment, and the values classify as non-GC, non-number table-internal
sentinels. FINREG missing-key insertion now uses `LJ_TKEYLOCK` as a transient
key publication marker while preserving its existing value-slot finalizer claim,
and the C table hash lookup/traversal bridge now filters `LJ_TKEYLOCK` with the
bounded retry rule below. C hash getters now hop from a matched
`LJ_TFORWARD` value to `TabNodeHdr.next_gen` when a successor is published,
falling back to the earlier one-shot retry/absence behavior if no successor is
visible yet. Resize counting/copying, traversal, and length helpers treat
`LJ_TFORWARD` values as absent internal migration markers so they are not
exposed or recopied by the current bridge. Integer array getters now hop a
forwarded separated-array slot through `TabArrayHdr.next_gen`, falling through
to integer hash lookup when the successor array is too small for a shrink-tail
key. C setter lookups route matched forwarded array/hash slots to successor
generations when `next_gen` is visible, including integer hash keys that
migrated into the successor array; if the successor is still unresolved after
the bounded retry, setters now repair/rebuild and re-resolve instead of
returning the internal sentinel slot to callers. C `lj_tab_next()` now resolves
visible forwarded array/hash slots through the published successor for the
current logical key.
x64 `ipairs_aux` now routes visible forwarded array slots through the exported
C integer getter wrapper before deciding whether the iterator stops. x64 array
`BC_TGETV`, `BC_TGETB`, and `BC_TGETR` now send visible forwarded slot
snapshots to the existing vmeta/C lookup path instead of returning the internal
sentinel. x64 `BC_TGETS_Z` does the same for visible forwarded string-key hash
values. x64 `BC_ITERN` delegates visible forwarded array/hash iterator slots to
the C `lj_tab_next()` path through a traversal-index helper, so optimized
`pairs()` loops no longer copy internal forward sentinels into Lua results.
x64 `lj_vm_next` delegates visible forwarded array/hash iterator slots to the
same C `lj_tab_next()` resolution path and returns the resolved value/key
layout expected by JIT-recorded `next()`.

## 6.2 Tables: data structures

GCtab (lj_obj.h:498–511) is reshaped for the lockless runtime:
```c
typedef struct GCtab {
  GCHeader; uint8_t nomm; int8_t colo;     /* colo: dead here (=0)      */
  MRef arrayhdr;     /* -> AHdr, replaces array+asize                   */
  GCRef gclist;      /* (kept: weaklist linkage uses gcw now; keep slot */
  GCRef metatable;   /*  for layout stability of metatable offset!)     */
  MRef  nodehdr;     /* -> NHdr, replaces node+hmask+freetop            */
  uint32_t asize_c;  /* cached asize copy for JIT guards (==ahdr->asize)*/
  uint32_t hmask_c;  /* cached hmask copy (==nhdr->hmask)               */
} GCtab;             /* still 64B/4 cells; offsets of metatable kept    */

typedef struct AHdr { uint32_t asize; uint32_t flags;
                      TValue slots[]; } AHdr;
typedef struct NHdr {
  uint32_t hmask;            /* size-1, power of 2                      */
  uint32_t resize;           /* 0 idle | 1 copying (CAS-claimed)        */
  MRef next_gen;             /* -> next NHdr during copy                */
  _Alignas(8) uint32_t freecount;  /* atomic countdown of fresh nodes   */
  uint32_t copy_cursor;      /* fetch_add migration cursor              */
  Node nodes[];              /* hmask+1 nodes; Node unchanged 32B       */
} NHdr;
```
`asize_c/hmask_c` exist because HREFK/asize guards in traces want a flat
field at a fixed GCtab offset (08 §8.8.2); they are updated (release) at
the same instant the gen pointer is republished and may lag — every guard
that uses them is backed by a re-check through the real header on the slow
path, so staleness only costs a trace exit, never correctness.

Empty hash part: the original report target is still that `lj_tab_newkey`
on hmask_c==0 goes straight to "install first NHdr" (a resize from nothing)
and deletes the freetop-in-nilnode trick. Current implementation is an
intermediate: empty tables still point `t->node` at the shared `g->nilnode`,
but `global_State.nilnodehdr` is embedded immediately before it, so every
published table node pointer has a valid header and writes must still rehash
before targeting shared nilnode.

## 6.3 Tables: operations

Notation: `AH = la_loadptr_acq(&t->arrayhdr)`, `NH = la_loadptr_acq(&t->nodehdr)`.
All run between safepoints (I-4); no operation below may allocate-with-GC
mid-protocol except where stated (allocation happens *before* entering the
publish step).

### 6.3.1 Array read `t[i]`, 1 ≤ i ≤ asize
```
AH = acq(t->arrayhdr); if i <= AH->asize:
  v = tv_rawload(&AH->slots[i-1])
  if itype(v)==LJ_TFORWARD: AH2 = acq(t->arrayhdr); retry in AH2  (≤1 hop:
      forwarding is only installed in retired gens; new gen never forwards)
  return v (nil ⇒ fall through to metatable path as today)
else: hash part lookup with integer key (as today, lj_tab.c getinth path)
```
### 6.3.2 Array write
```
AH = acq; if i <= AH->asize:
  wbarrier(v); old = la_xchg64? NO — plain tv_rawstore… but must not lose
  vs. migration: store with CAS-loop: do { o=tv_rawload(slot);
    if (o==FORWARD) goto newgen; } while(!la_cas64(slot,&o,v));
  (On the fast path — no resize ever started for this gen — the CAS
   succeeds first try; cost vs plain store: lock cmpxchg vs mov. To keep
   the common case a plain store: gens carry AHdr.flags bit AF_RETIRING set
   *before* any FORWARD is installed; write path: if(!(flags&RETIRING))
   plain store else CAS-loop. flags load folds into the asize load (same
   cache line). DECIDED: flag-gated plain store.)
else: grow path → §6.3.5 array growth, or hash insert per rehash heuristics
```
### 6.3.3 Hash lookup (lj_tab_get/getstr/getinth)
Unchanged probe logic minus relocation: `n = &NH->nodes[hash & NH->hmask]`,
walk `n->next` (acquire loads). Key compare: load key (rlx); KEYLOCK ⇒
treat as mismatch-and-continue, then one re-walk of the chain before
declaring miss (an installer may have completed; bounded: one retry —
proof: KEYLOCK only precedes a key that hashes to this chain; re-walk after
acquire-load of chain head observes the completed install or the slot
reverted). val==nil ⇒ absent (tombstone semantics as today). val==FORWARD ⇒
continue in `acq(NH->next_gen)` from the top.
### 6.3.4 Hash insert / set (lj_tab_set/newkey rewrite)
```
find existing node with key (6.3.3, in newest gen reachable):
  hit: wbarrier(v); store val per the RETIRING rule of §6.3.2
  miss in gen G (G->resize==0 path):
    main = &G->nodes[h & G->hmask]
    if key-claim(main): install there            # CAS key nil→KEYLOCK→
    else: idx = claim_free_node(G)               # atomic freecount--
          if none → start_resize(G) → retry whole op in new gen
          node = &G->nodes[G->hmask + 1 - idx]   # fresh nodes from tail?
          NO: fresh-node region = nodes never used as main position can’t
          exist (every index is a possible main). DECIDED free-node claim:
          linear CAS scan for key==nil starting at h, wrapping, bounded by
          freecount>0 guarantee (model file implements & measures: avg <2
          probes at ≤75% fill).
          install key: CAS key nil→KEYLOCK (claim), tv_rawstore val=nil,
          wbarrier(key); tv_rawstore_rel key=K (publish),
          then link: do { old=acq(main->next… chain head is main’s next?
```
Chain anchoring (DECIDED, matches model): every chain is anchored at the
main-position node. If main node's key is nil, the inserter claims main
itself (no link needed). Otherwise the new node is CAS-prepended into
`main->next` (release CAS on the MRef as uint64). Readers acquire-load
links. Nodes are never unlinked or moved within a gen (I-5); deletion =
val:=nil only (exactly Lua's semantics; space reclaimed at next resize).
Brent's relocation (lj_tab.c:441–495) is deleted; its job (main-position
purity) is now done by resize-time rebuild. Consequence: chains can hold
foreign-hash nodes ⇒ slightly longer average chains pre-resize; resize
trigger compensates: resize when freecount==0 OR chainlen>LJ_MAXCHAIN(=8)
observed during insert.
### 6.3.5 Resize / rehash (cooperative, lock-free)
```
start_resize(t, G):
  pre-allocate G2 (size by rehash heuristics ported from rehashtab
    lj_tab.c:357–374, counting via bitmap-free scan of G under no lock —
    counts may be stale; only sizing quality suffers)
  if !la_cas32(&G->resize, 0→1): G2 was wasted → defer_free(G2); help below
  rel-store G->next_gen = G2; set t-level hint flag (AHdr RETIRING for
    array shrink case set here too)
  MIGRATION (initiator + any thread that lands in 6.3.4-miss during copy
    helps): loop idx = fetch_add(&G->copy_cursor) while idx <= hmask:
      node = &G->nodes[idx]
      freeze: do { v = tv_rawload(&node->val) }
              while (!la_cas64(&node->val, &v, FORWARD))
      if v != nil and key != nil/KEYLOCK:
        insert (key,v) into G2 via 6.3.4 (cannot recurse-resize: G2 sized
        for full load; assert)        # wbarrier(key,v) inside
  array part migrates the same way when growing/shrinking asize (slots
  frozen with FORWARD after copy; readers hop once per 6.3.1)
  PUBLISH: rel-store t->nodehdr=G2 (and arrayhdr if changed);
           update asize_c/hmask_c (rel); defer_free(G) per 05 §5.9.
Writers during migration: any write that meets FORWARD or resize==1
  must first help-migrate *that key's* old node (freeze it), then write
  into G2 — guarantees no lost update (the freeze CAS linearizes old-gen
  writes against the copy; once FORWARD, the only writable home is G2).
Readers never help, never block.
```
Progress: lock-free — a stalled initiator cannot block others (helpers
advance cursor; publication is idempotent via CAS on t->nodehdr expected
G). Memory: at most 2 gens live per table transiently.

Current implementation status: runtime tables still use the legacy `GCtab`
layout and freetop scan while the tracked `nbtab_model` remains the reference
for the final header-generation/helper-copy port. As an NHdr-lite bridge,
hash vectors are now allocated with a small `TabNodeHdr` immediately before
`Node[0]`; `GCtab.node` still points at `Node[0]`, and `GCtab.hmask` remains a
compatibility mirror, but C-side hash-vector readers derive the real mask from
the acquired node header. The node header's second word now carries generation
flags: fresh node vectors and the shared nilnode start at zero, and
`lj_tab_resize()` sets `TABNODE_FLAG_RETIRING` on an old hash generation after
rebuild/routing is complete and before publishing replacement hash state.
`TabNodeHdr.next_gen` is release-published to the replacement hash vector or
shared nilnode before the old generation is marked retiring, reserving the
target needed by the final `LJ_TFORWARD` hop. The shared nilnode has a matching
embedded header with no successor.
The landed intermediate slices keep the shared nilnode read-only for first hash
inserts and retire old headered hash vectors plus separated legacy array
vectors from `lj_tab_resize()` behind the completed safepoint epoch before
freeing them. This removes the immediate resize free hazard for future
acquire-load readers and prevents C readers from indexing one node generation
with another generation's mask. Replacement hash vectors for non-empty resizes
are now rebuilt off-table before `GCtab.node` is release-published; old hash
entries are routed through the final array size first, so integer-like keys that
belong in the resized array still move there before remaining pairs are copied
into the unpublished replacement hash. Array-tail values that leave the array
during shrink are inserted into the unpublished replacement hash before the
smaller `asize` is published, and explicit undersized hash requests are raised
to the counted live hash payload after final array routing. Retirement of the
old hash vector is armed only after that rebuilt vector is published. Hash-chain
walks in the C runtime and serialization paths now use acquire loads, and
collision inserts initialize a stable free node before release-publishing it
through the anchor's `next` link; legacy Brent node relocation has been removed.
The FINREG-specific missing-key insertion bridge now release-stores
`LJ_TKEYLOCK` into claimed key slots and has FINREG inserters plus legacy/GC2
FINREG traversal wait out that key marker before treating the slot as
published; this is still scoped to the hidden FINREG generations, not the
general hash insert protocol above. C hash lookup paths now pause and re-walk
once when they see `LJ_TKEYLOCK`, setter probes retry directly, and
`lj_tab_next()` hides a still-locked key from traversal. The legacy
`lj_tab_newkey()` helper now defensively rechecks the target collision chain
and waits on `LJ_TKEYLOCK`; general new-key insertion KEYLOCK-claims nil
anchor/free-node keys before publishing and avoids overwriting non-nil
tombstone anchor keys. Collision insertion uses a bounded linear CAS scan
instead of mutating the legacy `freetop` cursor in `lj_tab_newkey()`.
`TabNodeHdr` now packs an atomic free-node counter into the low bits of its
state word, and shared `lj_tab_newkey()` plus FINREG insertion reserve/release
that counter around anchor/free-node claims. Anchored collision insertion also
triggers a hash rebuild/grow when the observed chain has reached
`LJ_TAB_MAXCHAIN`, so the bridge now covers the plan's freecount-empty and
chain-too-long resize triggers. Cooperative resize/helping is still pending. The
shared C array snapshot helper now retries when a selected separated array
generation is marked
`TABARRAY_FLAG_RETIRING`. Shared C hash readers that only need a node/mask pair
now use `lj_tab_node_snapshot_acq()`, which reacquires when the selected node
generation is marked `TABNODE_FLAG_RETIRING`; write-side set/new-key/FINREG
claim probes now use the same helper before mutating a hash generation. These are
flag-consumption bridges before the final `LJ_TFORWARD` copy/hop protocol.
The legacy `GCtab.node` pointer itself is now release-published after vector
initialization and acquire-loaded by C-side table, GC, serialization,
bytecode-writer, parser, and recorder readers. Legacy `GCtab.array` C readers
use acquire-loaded pointer/size helpers and snapshot slot values before
nil/type/copy decisions; array growth initializes slots before release-storing
the pointer and then `asize`, while shrink keeps the current
allocation capacity until the final `AHdr` port. Integer `lj_tab_getint()` /
`lj_tab_setint()` access now snapshots the array pointer with the legacy
`asize` mirror and then uses `TabArrayHdr.asize` for separated array
generations, preserving the colocated-array path until the final `AHdr` port.
Core C array scanners for clearing, duplication, resize counting,
traversal/length, bytecode/serialization/parser walks, serializer dictionary
prep/decode lookups, recorder table traversal type prediction, and legacy/GC2
marking now share that snapshot helper instead of independently pairing
`GCtab.array` with the legacy size mirror. Linux/x64 shared-array recording also
uses the same record-time snapshot to decide whether the current table shape has
a separated array header, and separated shared numeric miss/extension guards now
load `TabArrayHdr.asize` before continuing to `HREF`. The current separated
array header keeps the interim 8-byte slots-prefix shape but now packs capacity
and reserved state into `TabArrayHdr.acap`: `TABARRAY_ACAP_MASK` preserves the
plain capacity for existing readers, while `TABARRAY_FLAG_RETIRING` reserves
the first flag bit for the final §6.3.2/§6.3.5 write/migration protocol.
`lj_tab_resize()` now sets that bit on the old separated array generation after
replacement contents are ready and before publishing replacement table state;
current generations still start with zero flags until their own resize.
`TabArrayHdr.next_gen` is release-published to the replacement separated array
before the old array is marked retiring, reserving the successor needed by the
final array `LJ_TFORWARD` hop. C integer array getters use that successor when
they observe a forwarded slot in an old separated array generation, and fall
through to the integer hash lookup if the successor array no longer covers the
key.
On x86-64,
`getmetatable`'s `__metatable` probe, `ipairs_aux` empty-hash fallback,
`lj_vm_next` hash traversal, `BC_TGETS_Z`, and `BC_ITERN` hash traversal now
load the mask from the acquired node header instead of the legacy
`GCtab.hmask` mirror. `ipairs_aux`, `lj_vm_next`, and `BC_ITERN` also load
array bounds from `TabArrayHdr.asize` for separated array generations, with
the legacy `GCtab.asize` mirror kept as the empty/colocated-array fallback.
The same separated-array header bound check now covers x64 `BC_TGETV`,
`BC_TGETB`, and `BC_TGETR` array read fast paths, plus `BC_TSETV`,
`BC_TSETB`, `BC_TSETR`, and `BC_TSETM` array write fast paths before their
release-publishing store helpers. x64 `BC_TSETV`, `BC_TSETB`, and `BC_TSETR`
now slow-path visible array `LJ_TFORWARD` slots through the C setter bridge
instead of publishing into the old generation.
Those x64 array fast paths now take the legacy `GCtab.asize` snapshot before
loading `GCtab.array`, matching the writer-side publication order that stores
the array pointer before the size mirror. Separated arrays still replace the
legacy bound with `TabArrayHdr.asize` after the pointer snapshot.
`BC_TSETS_Z` string-key stores are currently demoted to
`vmeta_tsets`, removing the x64 VM's direct string-key hash-chain store. The
generic x64 `vmeta_tset` continuation release-stores returned slots through
`lj_meta_tsettv_pair()` with resolved parent context. That helper now
CAS-publishes the resolved slot and retries `meta_tset()` if the slot became
`LJ_TFORWARD` after lookup but before publication, so the VM/meta slow store
does not erase a migration sentinel. The broader C-side table setter and JIT
helpers still await the original flag-gated RETIRING/FORWARD/CAS write
protocol for migration correctness. The x64 VM hash readers that consume
`TabNodeHdr.hmask` (`getmetatable`, `ipairs_aux`'s hash fallback,
`lj_vm_next`, `BC_TGETS_Z`, and `BC_ITERN`) now test
`TABNODE_FLAG_RETIRING` and leave the fast path instead of traversing a retired
hash generation. Regular x64
dynamic `IR_HREF` lowering also uses the node-header mask instead of
`GCtab.hmask`, and x64 `IR_HREF`/`IR_HREFK` lowering exits on
`TABNODE_FLAG_RETIRING` before traversing a retiring hash generation;
constant-slot HREFK lowering also has an interim node-header bounds guard
before reading its recorded slot. Linux/x64 helper-backed `ASTORE` and
`HSTORE` lowering now release-publishes through table-store helpers with parent
barrier context, including shared table references and numeric `NEWREF` stores,
and those helpers resolve visible forwarded destinations through successor
generations when the destination belongs to the parent's current array or hash
generation. The traced `ASTORE`/`HSTORE` helpers now receive the array index or
hash key and resolve before CAS if the recorded slot is no longer in the
parent's current generation; the `NEWREF` helper key-resolves before every CAS.
This prevents helper-backed traces from publishing into stale retiring slots
that have not yet become visibly forwarded. The final generated
RETIRING/FORWARD/CAS write protocol remains pending.
`lj_vm_next`
hash traversal, the `BC_ITERN` array/hash iterator path, and `ipairs_aux` array
iteration load candidate values into registers before nil decisions and copy
those same snapshots to their results; `BC_TSETV`/`BC_TSETS`/`BC_TSETB` load
previous slot values into registers before nil/metamethod decisions and the
array paths reject forwarded snapshots before stores. x64
`BC_TSETV`/`BC_TSETB`/`BC_TSETR` now derive the destination slot from the same
array pointer whose header supplied the separated-array bound. x64 `BC_TSETM`
keeps its constructor fit check and `lj_tab_reasize()` retry, performs the fit
check against `TabArrayHdr.asize` for separated arrays, and batch-publishes
copied result slots through `lj_tab_storetvn()` instead of the old raw
`mov [array], TValue` loop. C-side runtime/library/parser/serializer
table-slot writers converted so far publish values through `lj_tab_store*()`
helpers or `copyTVrel()` instead of raw `lj_tab_set*()` destination stores;
parser anchor stores, serializer dictionary preparation, and recorder template
markers also back off through no-`lua_State` sleeps when a forwarded slot or CAS
loss forces a retry; central VM/JIT table-store helper retries and CAS losers
use the same no-`lua_State` wait helper;
rehash/new-key insertion also release-publishes hash keys through
`tab_storekeyrel()` and moved values through `copyTVrel()` during legacy resize
rebuilds. This records a scoped release-store bridge without changing the
original write-protocol target above. Core C table lookup, resize, rehash
counting, collision checks, and `next()` now make key/value
decisions from acquired `TValue` snapshots instead of direct shared node-field
reads; GC/GC2 table traversal, weak clearing, finalizer-table scans,
serialization, bytecode writing, parser template-table fixup, recorder
traversal typing, recorder template-table growth scans, and `table.maxn` use
the same snapshot helpers. These steps do not replace the legacy resize
algorithm with the planned lock-free `AHdr`/`NHdr` generation protocol yet;
the original RETIRING/FORWARD/CAS helper-copy design above remains the target,
and resize copying is still a non-cooperative legacy-`GCtab` operation. C-side
table hash getters hop through `TabNodeHdr.next_gen` on a visible
`LJ_TFORWARD` matched slot, or retry once and report absence if no successor is
available yet, while resize counting/copying, `next()`, and length helpers
filter `LJ_TFORWARD` values as absent internal sentinels. C integer array
getters likewise hop visible array `LJ_TFORWARD` slots through
`TabArrayHdr.next_gen`, including falling through to the hash part after a
shrink-tail hop. Integer hash lookups that observe a forwarded old hash slot
also check the successor array after a hash-generation hop, covering array
growth that migrates the integer key out of the hash part. C `lj_tab_next()`
resolves visible array and hash
`LJ_TFORWARD` slots through the published successor generation for the current
logical key before falling back to absent filtering, and x64 `BC_ITERN` plus
`lj_vm_next` delegate visible forwarded iterator slots to that C path. C length
helpers follow `TabArrayHdr.next_gen` for visible forwarded array slots before
deciding whether a candidate boundary is absent.
C setter lookups now route matched visible `LJ_TFORWARD` slots to successor
array or hash generations when `next_gen` is already published. When the
successor is still unresolved after the bounded retry, setters repair the
current generation by forcing a replacement array, moving the key to the hash
part in the max-array edge case, or rebuilding the hash generation, then
re-resolve the key instead of returning an internal sentinel slot to a caller.
This is a safety bridge, not the final cooperative RETIRING/FORWARD/CAS writer.
Publication barriers that receive a `TValue *` snapshot the value before GC2
marking and legacy `tviswhite()` / `gcV()` checks, so the current release-store
bridge does not reread a shared destination slot after publication.
### 6.3.6 next/pairs (lj_tab_next)
Iterate the *gen snapshot* captured at first call: store the NH pointer in
the iterator control slot? Lua's `next(t,k)` is stateless — DECIDED:
re-derive each call from key position in the newest gen (as today: key →
node index, scan forward). Under concurrent resize an in-flight iteration
may restart positions; guarantees (per 02 M-4): keys present throughout are
each visited ≥ once per gen they occupy… To honor "never visits a key
twice" cheaply across a gen swap, BC_ITERN's hidden control already encodes
an index; on detecting gen change (compare cached hmask_c snapshot in the
control slot's high bits — there are spare bits in the ITERN control
encoding, see vm_x64.dasc BC_ISNEXT/ITERN) fall back to `next` semantics
"undefined after modification" — which is *already* Lua's contract when the
table is mutated during traversal. So: implement the simple re-derive,
document that concurrent resize ⇒ standard "modified during next" caveat,
keep crash-freedom via the normal read protocol. (FORWARD encountered by
next ⇒ hop to new gen at same logical position.)
### 6.3.7 lj_tab_len (#), metatable, nomm
`#`: today's array binsearch over AH snapshot — returns a border (M-4).
metatable: original report target was rel store + wbarrier with relaxed readers
(mm dispatch tolerates stale); the current bridge acquire-loads
release-published metatable/env refs in GC, GC2, metamethod, finalizer,
serialization, threading-environment, public API/library, loader, closure
inheritance, and recorder C readers so the release/read contract is explicit
before the full generation-table protocol lands.
nomm: advisory negative cache; in concurrent table access it may go stale when a metatable
gains a method ⇒ setmetatable and rawset-into-metatable clear nomm of —
impossible to find all referrers; instead: nomm is only trusted when
metatable ptr also matches the cached… DECIDED minimal-risk: in the lockless runtime,
`nomm` is set ONLY at table creation for the no-metatable case and cleared
on setmetatable; mm-presence fast checks degrade to metatable-load + method
lookup when a metatable exists (same as interpreter slow path today; JIT
guards on metatable identity anyway, 08 §8.8.3). Cost: metamethod-heavy
interpreter code loses one shortcut; benchmark gate covers it. Current bridge
code snapshots Lua and ctype metamethod table slots with acquire loads before
tag checks, copies, finalizer decisions, or recorder metadata caching; the
original per-generation table protocol above remains the final table-storage
target.

## 6.4 Upvalue cells (the closed-only model)

### 6.4.1 Runtime object
GCupval (lj_obj.h:432–445) in the lockless runtime: `closed` is always 1, prev/next
(open-list) deleted (header gcw free), `v` always points at own `tv` —
collapse: keep struct layout but assert closed; `uvval(uv) == &uv->tv`
becomes compile-time. Mutation: `tv_rawstore` + wbarrier
(replaces lj_gc_barrieruv, lj_gc.c:819). dhash retained (JIT aliasing).
Current implementation keeps the transitional struct/list shape, release-copies
close-time payloads into `uv->tv`, and has legacy GC/GC2 acquire-snapshot
`GCupval` and C-closure upvalue `TValue` payloads before marking.
`lj_gc_pubuv()` and close-time gray repair likewise run barrier decisions from
an acquired snapshot of the closed-cell payload rather than inspecting the
shared cell directly. C helpers that copy C-closure upvalues out to a stack
snapshot each shared `TValue` individually; the `io.lines()` iterator option
copy is the current guarded instance.
### 6.4.2 Compiler (lj_parse.c) — DECIDED scheme
The parser already computes, per function, which locals are captured
(`var_lookup`/`fscope_uvmark` machinery marking VSTACK entries). Add a pass
at `fs_finish` time or eagerly:
- A local that is captured anywhere becomes a **cell-capable local**:
  source/v4 FNEW promotes the raw parent slot to a closed GCupval cell when a
  closure is actually created. Original target: owner-frame accesses discovered
  after capture use `CGET dst, slot` and `CSET slot, src`; those opcodes tolerate
  raw slots before promotion, which keeps conditional closure creation and
  loop-variable updates correct. Current implementation is deliberately more
  conservative: source owner-frame local reads/writes are emitted as raw-tolerant
  `CGET`/`CSET` from the start, because one-pass capture discovery can happen
  after earlier bytecode that later re-executes after `FNEW` promotion.
- Child FNEW upvalue descriptors: the proto uv table entry for a
  local-capture (today flagged PROTO_UV_LOCAL with slot index — see
  lj_parse var_add/uv handling and lj_func.c:func_finduv use) now means
  "use a closed cell for parent slot N". Mutable captures promote a raw
  parent slot to an LJ_TUPVAL cell in place, or inherit an existing cell; owner
  bytecode observes that cell through `CGET`/`CSET` rather than raw slot ops.
  Immutable captures may snapshot a raw parent slot into a closed cell without
  replacing the owner slot; this preserves earlier owner-frame bytecode emitted
  before the parser later discovers the capture. Upper-level captures
  (parent's upvalue) unchanged: copy parent's uvptr entry.
- UCLO: v4 source no longer emits closing UCLO with `A != 0`; UCLO 0 remains
  as a return/jump carrier pending a later cleanup. `fscope_end`/goto
  resolution keep ordinary JMPs for scope exits.
- Slot content invariant: before closure creation the slot may hold the raw
  value; after FNEW/CNEW promotion it holds the LJ_TUPVAL-tagged ref until a
  loop opcode overwrites it with the next raw iteration value. Debugger
  `debug.getlocal` unwraps cell slots. Loop capture exactness comes from loop
  opcodes overwriting the visible slot with a new raw value before each body
  iteration, then FNEW promoting that iteration's value when a closure is made
  (test t-uv-07).
### 6.4.3 Interpreter & JIT
vm handlers: 07 §7.6 (CNEW allocates via inline bump; CGET = load slot,
deref uv->tv; CSET = load slot, wbarrier, store uv->tv). JIT: 08 §8.8.4
(CGET→UREFC+ULOAD and CSET→UREFC+USTORE+barrier; CNEW participates in
allocation sinking at M9). Current implementation has x64 direct-cell
recorder/lowering support for owner-frame CGET/CSET, including raw-slot
fallback before promotion and dispatch-table fixes for recording/hook
redispatch. Source child protos and loaded v4 child protos with parent-cell
upvalues can trace through normal closed-upvalue UGET/USET recording after
FNEW promotion; loaded v4 owner CGET/CSET protos can trace on the same x64
owner-cell path. Original plan/WIP wording kept all loaded v4 cell protos
`PROTO_NOJIT`; the audited boundary is narrower. Self-captured
local-function CNEW/FNEW/CSET source protos and loaded v4 protos containing
the same self-cell shape can now trace through the first helper-backed M6
slice. Mixed raw-local FNEW traces are covered for source/loaded immutable
captures through stack-value synchronization and for mutable captures after the
slot is promoted at trace entry or when the hot trace performs the first
mutable raw-slot promotion with otherwise type-stable loop-carried slots.
### 6.4.4 Legacy chunks: see 10 §10.4 (capture-at-FNEW under MT).

## 6.5 String interning (lj_str.c rewrite)

### 6.5.1 Structure
```c
typedef struct StrTabHdr { uint32_t mask; uint32_t resize;
  uint32_t copy_cursor; uint64_t pad; GCRef bucket[]; } StrTabHdr;
g->str.tabh : StrTabHdr*  (RCU, acq/rel)        /* replaces tab+mask */
```
Chain links live in each GCstr's `gcw` (was nextgc; 04 §4.7). Bucket head
and links carry a **Harris mark**: low bit set = string logically dead
(sweep-claimed). The secondary-hash scheme (LUAJIT_SECURITY_STRHASH,
lj_str.c chain bit) is preserved: the *second-lowest* bit of the bucket
head encodes "secondary chain" (the original report used bit0; the current
implementation moved it to bit1 and reserves bit0 for the future Harris
dead-link mark).
### 6.5.2 Intern (lj_str_new)
```
h = hash(...); walk bucket (acq loads), skipping Harris-marked nodes:
  match found (hash,len,memcmp):
     if P_SWEEP active: resurrect = la_bit_test_and_set64(markbit) — and
     re-check node not Harris-marked (acq reload of its link). If it became
     marked-dead: treat as miss (it is provably unreachable from Lua —
     fixpoint ended before sweep — so identity I-8 is preserved by making a
     fresh string). Else return it.
  miss: allocate GCstr (alloc fast path; alloc_black per phase), fill, sid
     = la_add32(&g->str.id,1) (StrID uniqueness kept; idreseed handled at
     resize under the resize claim), CAS bucket head old→new (rel) with
     new->gcw=old. CAS failure ⇒ re-walk from new head: if an equal string
     appeared, abandon ours (it is garbage; never published — no identity
     leak) and return theirs; else retry CAS. Wins are bounded lock-free.
resize trigger: num > mask (la_add32 num on success): claim via
  CAS(tabh->resize 0→1); copy buckets with cursor fetch_add; insertions
  during copy go to BOTH? No — DECIDED single-home: inserts during copy
  install into the NEW table; lookups during copy check OLD then NEW
  (ordering: new-then-old would miss in-flight migrators; old-then-new with
  migration moving head-first under CAS… simplest sound rule, model-tested
  analog in nbtab: lookup checks BOTH tables while tabh->resize==1).
  Publish new hdr (rel), defer_free old. Dedupe across the window: an
  insert must double-check the other table before publishing (walk both;
  the CAS-on-head linearizes within a table; cross-table race window is
  closed by: inserts during resize CAS into NEW only, and a concurrent
  pre-resize insert into OLD is migrated by the copier *after* it lands —
  copier re-runs the dedupe walk on NEW per moved string and drops losers
  to defer_free… losers were published! Identity break. FINAL RULE
  (DECIDED, simple, correct): resize claims also gate inserts — inserters
  observing resize==1 *help finish the copy first*, then insert into NEW.
  Helping is bounded (cursor); still lock-free (initiator stall ⇒ helpers
  finish). This serializes intern-vs-resize without a lock.)
Current implementation status: bucket publication now uses release CAS under
an active-user pin on `StrTabHdr.resize`. Resize claims the high bit even when
active interners are present, then drains the active count before copying and
publishing the replacement header; new entrants spin on the claimed bit. The
old header is retained on a raw retired-header list and freed after a later
completed safepoint handshake epoch, avoiding immediate RCU use-after-free for
threads that loaded the old header before pinning it. The string count is
updated and read with atomic helpers (`la_add32_rlx`, `la_sub32_acqrel`,
`la_load32_acq`) so sweep-side frees and shrink checks do not race plain
accesses against concurrent interns. `GCstr.sid` allocation uses
`la_add32_rlx(&g->str.id, 1)`; the old allocation-time `idreseed`/global-PRNG
mutation is deferred until it can be reintroduced without racing allocation
outside the active table pin. Secondary-chain rehash now reuses the same
claim/drain bit on the current header, verifies the header is still current
after the claim, rechains in place while new entrants spin, and releases the
bit before retrying the pending insert. The original full helping protocol
above is still the target; bounded helper copy, cross-table resize
participation, generic deferred-free buckets for all raw gens, and Harris
dead-link sweep remain follow-up work.
### 6.5.3 sid / idreseed: StrID wraps are handled as today at full-resize
points (lj_str.c:129+ logic) under the resize claim.
### 6.5.4 Sweep
A worker walks every bucket: for each unmarked, non-FIXED string: CAS its
predecessor link to set Harris bit on victim's link, then CAS-unlink
(standard Harris two-step; concurrent CAS-prepends at head retry on head
change). The GCstr memory itself is reclaimed by the normal arena sweep —
but ONLY after unlink + one grace epoch: string arenas are therefore swept
in a *second* sweep wave ordered after strtab unlink + epoch tick (leader
sequences: strtab-unlink → tick handshake → release string arenas to lazy
sweep). The §6.5.2 resurrect rule makes lookups during the window safe.

## 6.6 Buffers (lj_buf, string.buffer)
SBuf in TG (03). `string.buffer` objects: documented single-owner (sharing
one buffer object across threads without external sync = M-2 races but
memory-safe: their data ptr/len updates become rel/acq pairs — 3-line
change in lj_buf.h grow path). lj_strfmt users get tg-buf.

## 6.7 lua_State ownership word
`thr_owner` CAS protocol per 03 §3.7 / 05 §5.7.2. APIs that touch a foreign
L (lua_xmove, debug.* on coroutines) require claim or owner==self; else
error "thread busy". lua_xmove across OS threads: allowed when both claimed
by caller (typical pattern: parent moves args into a not-yet-started L).

## 6.8 Long tail (each is a one-liner to implement; all M5)
- registry/gcroot stores: release-publish + wbarrier; C-side gcroot readers
  acquire-snapshot the published root.
- getmetatable/setmetatable on udata/cdata basemt (gcroot): same.
- `GCudata.udtype`: specialized userdata publish the discriminator with a
  release store only after payload/metatable initialization; C-side GC/runtime/
  recorder readers acquire-snapshot the discriminator before branching.
- `CType.name`: fixed FFI type names are release-published by `ctype_setname()`
  and acquire-snapshotted by name lookup/parser/recorder/library readers. This
  is only the name-ref slice; `CTState.hash`, `top`, `next`, and table growth
  remain on the original FFI concurrency plan.
- `lua_concat`/lj_meta paths: use tg tmpbuf; barrier on results stored.
- rawequal/rawget/rawset: thin wrappers over §6.3 ops (already are).
- `os`, `io`: untouched (user-level objects; FILE* sharing is user's
  problem per requirement 4 analog); `os.date` → localtime_r etc. audit.
- `math.random`: per-TG prng; `math.randomseed` seeds calling thread only
  (doc note 09 §9.8).
- package.loaded / require: ordinary shared tables now safe; the *loader*
  itself (dofile of same module twice) gets a per-key in-flight guard via
  a normal Lua-level channel pattern in lib_package (small Lua change) —
  acceptable simplification: concurrent require of the same fresh module
  may run the chunk twice with last-publish-wins (documented).

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

Empty hash part: today t->node points at the shared `g->nilnode` and
freetop abuses it (lj_tab.c:99,114,265, lj_obj.h:648). Under MT a *write*
must never target shared nilnode; keep nilnode for reads (lookup miss path
unchanged) but `lj_tab_newkey` on hmask_c==0 goes straight to "install
first NHdr" (a resize from nothing) — delete the freetop-in-nilnode trick.

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
for the final header-generation/helper-copy port. The landed intermediate
slices keep the shared nilnode read-only for first hash inserts and now retire
old hash `Node[]` vectors from `lj_tab_resize()` behind the completed
safepoint epoch before freeing them. This removes the immediate resize free
hazard for future acquire-load readers. Hash-chain walks in the C runtime and
serialization paths now use acquire loads, and collision inserts initialize a
stable free node before release-publishing it through the anchor's `next` link;
legacy Brent node relocation has been removed. The legacy `GCtab.node` pointer
itself is now release-published after vector initialization and acquire-loaded
by C-side table, GC, serialization, bytecode-writer, parser, and recorder
readers. Core C table lookup, resize, rehash counting, collision checks, and
`next()` now make key/value decisions from acquired `TValue` snapshots instead
of direct shared node-field reads. These steps do not replace the legacy resize
algorithm with the planned lock-free `NHdr` generation protocol yet.
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
metatable: rel store + wbarrier; readers rlx (mm dispatch tolerates stale).
nomm: advisory negative cache; in concurrent table access it may go stale when a metatable
gains a method ⇒ setmetatable and rawset-into-metatable clear nomm of —
impossible to find all referrers; instead: nomm is only trusted when
metatable ptr also matches the cached… DECIDED minimal-risk: in the lockless runtime,
`nomm` is set ONLY at table creation for the no-metatable case and cleared
on setmetatable; mm-presence fast checks degrade to metatable-load + method
lookup when a metatable exists (same as interpreter slow path today; JIT
guards on metatable identity anyway, 08 §8.8.3). Cost: metamethod-heavy
interpreter code loses one shortcut; benchmark gate covers it.

## 6.4 Upvalue cells (the closed-only model)

### 6.4.1 Runtime object
GCupval (lj_obj.h:432–445) in the lockless runtime: `closed` is always 1, prev/next
(open-list) deleted (header gcw free), `v` always points at own `tv` —
collapse: keep struct layout but assert closed; `uvval(uv) == &uv->tv`
becomes compile-time. Mutation: `tv_rawstore` + wbarrier
(replaces lj_gc_barrieruv, lj_gc.c:819). dhash retained (JIT aliasing).
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
`PROTO_NOJIT`; the audited boundary is narrower: self-captured local-function
CNEW/CSET source protos and loaded v4 protos containing `BC_CNEW` remain
`PROTO_NOJIT` until CNEW snapshot/FNEW recording behavior is implemented.
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
threads that loaded the old header before pinning it. Secondary-chain rehash
now reuses the same claim/drain bit on the current header, verifies the header
is still current after the claim, rechains in place while new entrants spin, and
releases the bit before retrying the pending insert. The original full helping
protocol above is still the target; bounded helper copy, cross-table resize
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
- registry/gcroot stores: la_storeptr_rel + wbarrier (lj_api.c sites).
- getmetatable/setmetatable on udata/cdata basemt (gcroot): same.
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

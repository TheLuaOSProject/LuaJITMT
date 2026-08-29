# GC2 canonical string quarantine protocol (2026-07-10)

## Status and relationship to the runtime gate

This note specifies the protocol required to remove
`LJ_GC2_STRING_BODY_RECLAIM == 0` safely. It extends the analysis in
[GC2 runtime string reclamation safety gate](gc2-string-reclamation-safety-gate-2026-07-10.md).

The existing tagged-link, bounded sweep, table-header pin, and retired-body
machinery is useful scaffolding, but it is not yet the canonical-lifetime
protocol described here. In particular, an arena mark observation and a
`DEAD` incoming-edge CAS do not atomically arbitrate a generic Lua publication
against logical string death.

This is a design note, not a claim that the protocol has landed. Production
runtime body reclamation must remain gated until stages A through F and their
proof tests are complete.

The protocol is restricted to the currently supported x86_64 targets. It
preserves ordinary LuaJIT string pointer identity for every live reference and
does not require forwarding checks on the normal string read, comparison, hash,
or byte-access paths.

## Required outcome

For each byte sequence there must be at most one acquirable `GCstr *` identity.
This must remain true while:

- the main intern table is being tagged or unlinked;
- an equal string is concurrently interned;
- a stack, table, upvalue, JIT temporary, FFI side root, or native helper
  publishes a racing reference;
- an interner traverses a table generation which is being resized or retired;
- a JIT trace or native call temporarily borrows `GCstr` fields or `strdata()`;
- retirement metadata allocation fails; and
- a state closes or a worker is reconfigured from any string subphase.

Physical reclamation has a second, separate requirement: no reader may still
dereference the old body and the allocator may not reuse its address until the
string-acquisition grace has completed.

Canonical identity and physical lifetime are deliberately separate. Main-table
unlink is not logical death in this protocol.

## Why two ordinary graces are insufficient

The following execution is legal without a publication-state arbitration:

1. Reader `R` loads old canonical body `p` from a racing Lua slot or native
   borrow and pauses before publishing it.
2. GC tags and unlinks `p` after sampling its mark as zero.
3. Interner `N` observes a main-table miss and publishes equal body `q`.
4. `R` resumes and stores `p` into a strong Lua edge.

Now both `p` and `q` are live equal strings. A post-unlink grace can postpone
freeing `p`, but postponing physical free does not repair duplicate logical
identity.

An exact interner rescue does not close this schedule because `R` did not
acquire `p` through the intern-table edge. Rechecking the mark immediately
before unlink only narrows the window. A generic publisher can set the mark
between that check and the edge CAS.

The protocol therefore needs all three of the following:

1. a canonical quarantine reservation which remains discoverable after the
   main edge is removed;
2. one atomic lifecycle word on which rescue and logical death directly
   compete; and
3. read-side publication before a shared pointer load, plus precise native
   borrow tracking, so the lifecycle word itself cannot be read from reclaimed
   storage.

## Canonical domain

The canonical directory is the union of two lock-free indices:

```text
main string table  +  canonical quarantine table
```

The main table remains the fast path. A main-table hit performs no quarantine
hash lookup. A main-table miss must consult the quarantine table before it may
allocate or publish a new equal body.

The quarantine table is not merely a retire list. While its record is in an
acquirable state, it is an authoritative canonical reservation for the same
body. A rescued quarantined body is re-linked into the main table; it is never
replaced with an equal body.

The ordering is essential:

```text
publish quarantine record -> publish body Q state -> unlink main edge
```

There is consequently no interval in which an old canonical body is absent
from both indices.

## Proof-first data layout

### Per-string canonical word

Add one naturally aligned atomic `uintptr_t canon` to `GCstr`. The proof-first
implementation may increase `sizeof(GCstr)` by one word. All VM and JIT string
payload offsets are generated from `sizeof(GCstr)` and must be rebuilt and
verified. A later optimization may move the word to a lazy arena side plane,
but it may not weaken the state machine or its address-validation contract.

The low three bits encode state. Record-carrying states store an aligned
`StrCanonRec *` in the remaining bits:

```c
LIVE       = 0
CANDIDATE  = 1
QACTIVE    = (uintptr_t)rec | 2
QRESCUED   = (uintptr_t)rec | 3
QCLOSING   = (uintptr_t)rec | 4
QCOMMIT    = (uintptr_t)rec | 5
FREEING    = (uintptr_t)rec | 6
```

One GC2 cycle owner serializes cycle transitions. A cycle number is therefore
not required in this word. A stale `CANDIDATE -> LIVE` CAS which happens to
land in a later cycle only retains the string conservatively. Record-carrying
states cannot suffer the same ABA because `QCOMMIT` never returns to an
acquirable state and each record remains SMR-retired until its users drain.

If implementation experience invalidates that single-owner assumption, the
word must gain a generation; omitting it may not be justified merely by test
success.

### Quarantine record

`StrCanonRec` contains at least:

```c
typedef struct StrCanonRec {
  struct StrCanonRec *qnext;       /* Atomic quarantine-bucket link. */
  struct StrCanonRec *retired_next;
  GCstr *body;
  StrTabHdr *main_hdr;
  StrHash hash;
  MSize len;
  uint64_t main_unlink_epoch;
  uint64_t close_epoch;
  uint64_t q_unlink_epoch;
  uint32_t main_linked;            /* Atomic topology status. */
  uint32_t status;                 /* Atomic ownership/shutdown status. */
} StrCanonRec;
```

The exact packing may change, but these ownership facts must remain
discoverable. The record is fully initialized before either its quarantine
bucket link or the body `canon` word is published.

The existing `g->str.sweep_pending` slot remains the discoverable pre-CAS owner
from record allocation through stable quarantine/retire-list publication. A
shutdown drain must never have to infer whether an unlink owner vanished
between these publications.

### Quarantine table

`StrInternState` gains a lock-free quarantine hash header and mask. Bucket links
are exact atomic links. Resized headers and removed records use the same SMR
discipline as the main string-table headers.

Records are keyed by the immutable string hash, length, and bytes. A lookup
must pin the quarantine generation and record before reading the body bytes.
Record allocation must come from a pooled or batched, non-throwing TG-local
metadata allocator. One general allocator call per dead string is not an
acceptable final performance shape.

## Public internal operations

The implementation needs centralized operations rather than open-coded mark
and store sequences:

```c
void lj_str_read_enter(TGState *tg);
void lj_str_read_leave(TGState *tg);

enum LJStrPublishResult {
  LJ_STR_PUBLISH_SAME,
  LJ_STR_PUBLISH_SUBSTITUTE,
  LJ_STR_PUBLISH_RETRY
};

int lj_str_publish_prepare(lua_State *L, GCstr **sp);
int lj_str_publish_prepare_tv(lua_State *L, TValue *tv);
int lj_str_gc2_rescue(global_State *g, GCstr *s);

StrCanonRec *lj_str_q_lookup_acquire(global_State *g,
                                     const char *bytes, MSize len,
                                     StrHash hash);
void lj_str_q_lookup_release(TGState *tg, StrCanonRec *rec);
int lj_str_q_help_relink(lua_State *L, StrCanonRec *rec);
```

Names are illustrative. The behavioral contracts are mandatory.

`lj_str_publish_prepare*()` is a strong pre-publication operation. There may be
no safepoint, retry wait, allocation, or other quiescent transition between a
successful prepare and the destination release store or CAS. A retrying helper
must prepare again for each store attempt or keep the value in a per-TG pending
publication root which the scanner treats as strong.

Weak-only edge publication needs an explicit mode. A weak store must not become
an accidental permanent strong root, but a value copied from a weak edge onto a
stack or strong destination must pass the strong prepare operation. Weak clears
must be exact and complete before `QCOMMIT` can be proven.

## State machine and linearization points

### 1. New canonical publication

The constructor initializes the immutable body, `canon = LIVE`, GC type, and
allocation-black state before publication. The main bucket-head CAS is the new
canonical identity linearization point.

Two constructors racing for equal bytes still use the ordinary relookup and
bucket CAS. Neither is allowed to insert until both the main and quarantine
lookups have proved absence.

### 2. Candidate tagging

After observing mark zero, the sweep owner performs:

```text
CAS canon LIVE -> CANDIDATE
CAS exact incoming edge old -> old|DEAD
recheck mark and canon
```

`CANDIDATE` must be published before `DEAD`. The edge bit is a topology hint;
the canonical word is the liveness authority.

If a strong publisher wins `CANDIDATE -> LIVE`, that CAS is the rescue
linearization point. The tagger or later scanner clears `DEAD`. If edge tagging
fails because a prepend moved the incoming edge, the tagger either finds the
new exact edge or conservatively restores `LIVE`; it may not leave an
unowned candidate.

The post-tag mark recheck closes a marker which observed `LIVE` immediately
before the candidate CAS. The acquisition grace described below closes a
publisher which validated `LIVE` before tagging and has not yet reached its
store.

### 3. Quarantine reservation

For an unmarked tagged candidate:

1. allocate and initialize `StrCanonRec` without throwing;
2. publish it in the quarantine bucket;
3. publish it in `sweep_pending`;
4. CAS `CANDIDATE -> QACTIVE(rec)`.

The lifecycle CAS is the quarantine reservation linearization point. If a
publisher already changed the body to `LIVE`, the CAS fails, the record is
cancelled and SMR-retired, and the main edge remains canonical.

Metadata OOM never removes the canonical identity. It restores `LIVE`, clears
`DEAD`, and retries in a later cycle.

A quarantine lookup which sees a record before the body's CAS treats it as a
prepared, non-authoritative record and validates the body word. It cannot
return that record merely because its bucket link exists.

### 4. Main-table unlink

Once `QACTIVE(rec)` is visible, the sweep owner exact-CASes the incoming main
edge to the successor and publishes `rec->main_linked = 0`.

This CAS is a topology linearization point, not logical string death. The same
body remains canonical through the quarantine record.

The main-miss ordering has no absence gap:

- quarantine insertion happens before main unlink;
- before quarantine insertion, the equal old main edge is still linked and a
  correct exact traversal cannot report a true miss; and
- after main unlink, an acquire quarantine lookup sees the record before a new
  insertion may be attempted.

### 5. Rescue and re-link

The acquirable quarantine states are `QACTIVE` and `QRESCUED`.

A publisher or interner rescues with one of:

```text
QACTIVE(rec)  -> QRESCUED(rec)
QCLOSING(rec) -> QRESCUED(rec)
```

The successful lifecycle CAS is the rescue linearization point. The mark is
then set. Returning or publishing the old body before that CAS succeeds is
forbidden.

If the body is still linked in the main table, a helper clears `DEAD`. If it is
detached, a helper scans both indices and CAS-inserts the same body into the
main bucket. Since the quarantine record reserved the byte sequence, no equal
replacement is allowed to have been inserted. The same-body bucket CAS is the
re-link topology linearization point.

After main re-link is visible, remove the quarantine record behind a record
grace, then publish `canon = LIVE`. `QRESCUED` is itself acquirable, so callers
need not wait for the re-link helper to finish.

### 6. Quarantine close and logical death

An unmarked `QACTIVE` body begins close with:

```text
QACTIVE(rec) -> QCLOSING(rec)
```

The owner records close boundary `E1` and runs a real string-acquisition grace.
Every operation which could have acquired the old pointer before `E1` must
either rescue it or publish quiescence. A new quarantine lookup which observes
`QCLOSING` may still attempt `QCLOSING -> QRESCUED`.

After the grace, the collector attempts:

```text
QCLOSING(rec) -> QCOMMIT(rec)
```

This CAS is the logical-death linearization point. Rescue and death compete on
the same word:

- if rescue wins, close aborts and the same body is re-linked;
- if commit wins, the old body can never again be returned or published.

An arena mark sample is not sufficient to authorize this transition.

### 7. Replacement after commit

Once `QCOMMIT` is visible, a quarantine lookup treats the old record as
logically absent. An interner may construct and publish a new equal body; its
main bucket CAS is the replacement canonical linearization point.

There are no two live equal identities. The old body became non-acquirable at
`QCOMMIT` before the new body could be published.

A rare late publication prepare which observes `QCOMMIT` or `FREEING` must
reload its source or re-intern the protected old bytes and substitute the
current canonical body. It must never expose the old pointer.

### 8. Directory removal and physical free

After `QCOMMIT`:

1. exact-unlink the quarantine record;
2. record boundary `E2`;
3. run a second acquisition and native-borrow grace;
4. CAS `QCOMMIT(rec) -> FREEING(rec)`;
5. claim `gct`, free the old body, and retire the record;
6. keep the allocator address quarantined until the reuse boundary is proven.

`FREEING` is terminal. It cannot be rescued or forwarded back to a live old
identity.

## Strong publication preparation

The prepare operation follows this state behavior while the caller is inside a
string read-side section:

| Observed state | Required action |
| --- | --- |
| `LIVE` | Mark, acquire-revalidate `canon`, then allow the store. |
| `CANDIDATE` | CAS to `LIVE`, mark, then allow the store. |
| `QACTIVE` | CAS to `QRESCUED`, mark, then allow the store. |
| `QCLOSING` | CAS to `QRESCUED`, mark, then allow the store. |
| `QRESCUED` | Mark and allow the store. |
| `QCOMMIT` | Reload or substitute the current canonical body. |
| `FREEING` | Reject the stale body without dereferencing its payload. |

The `LIVE` revalidation alone does not pin the state until a later store. The
read-side acquisition grace supplies that missing interval: a publisher which
validated `LIVE` before candidate tagging cannot be passed by `E1` until it has
stored, rescued, or quiesced. A publisher which begins after the candidate is
visible must compete through the lifecycle CAS.

Root and marker paths must use the same rescue operation. For strings,
`lj_gc2_markobj()`, `lj_gc2_preserve_sweep_root()`, stack scans, table scans,
upvalue scans, JIT trace roots, FFI side roots, and pending publication roots
may not merely set the arena mark bit.

Discovering `QCOMMIT` from an alleged strong root is a violated grace invariant.
Assertion builds must stop at that point. Production builds must fail closed by
retaining/quarantining the body and scheduling slot repair; they may not free a
body which a strong root still names.

## Read-side epoch contract

Entering after a pointer load leaves a pre-acquisition UAF window. The read
epoch must be visible before any shared `TValue` or raw `GCstr *` source is
loaded.

Each TG publishes at least:

```c
uint32_t str_read_depth;
uint64_t str_read_epoch;
uint32_t str_native_borrow_depth;
uint64_t str_native_borrow_epoch;
```

The exact layout may combine fields, but native remote acknowledgement must
remain distinguishable from actual string quiescence.

### Interpreter and C API

The outer VM/API entry publishes the current read epoch before its first shared
value load. At a real VM poll or safepoint it:

1. spills or publishes live VM values;
2. leaves or rolls the old string read epoch;
3. acknowledges and applies the root scan;
4. re-enters the current epoch before resuming value loads.

An executing VM may not be declared string-quiescent by a synthetic remote
acknowledgement.

The normal Lua C API rule remains: a pointer returned by `lua_tolstring()` is
kept valid by its Lua stack value. API code must nevertheless be inside the TG
read scope while it obtains that value. Holding a raw pointer after removing
all Lua roots remains outside the public API lifetime contract.

### JIT

Trace entry publishes the string read epoch before loading trace roots, TValue
operands, `GCstr` headers, or string bytes. It should be ordered with or before
`jit_base`. Trace exit clears the read epoch only after snapshot restoration
has made every live value discoverable.

Every direct trace-side shared store which may carry a string needs a
prepare-before-store guard or helper. A post-store `TBAR` is too late.

As a safe staging boundary, trace entry may exit to the interpreter while a
`QCLOSING` window is active. That is not the final lockless-performance shape.
The final implementation keeps unrelated traces running and performs the
strong prepare only on publication paths which need it.

JIT constants, IR/snapshot strings, trace side roots, and executable native
users must all be included. An executable trace which embeds or borrows a
string is either a marked semantic owner or an active epoch reader.

### FFI and native byte borrows

Remote acknowledgement of `in_native` is not string quiescence. A TG begins a
native string-borrow epoch before deriving or exposing `strdata()` and ends it
after the last native byte access.

The string close/reuse grace ignores a remote-native ACK while
`str_native_borrow_epoch <= boundary`. Other GC work may proceed; only the
affected string retirement boundary is deferred.

The first safe implementation may conservatively bracket an entire Lua C or
FFI call. The performance implementation should use a small exact per-TG hazard
set with an epoch overflow mode. Required audits include:

- FFI string-to-`char *` argument marshaling through native return;
- callbacks which temporarily leave and re-enter a native region;
- loader calls such as `dlopen`, `LoadLibrary`, and symbol lookup;
- internal helpers which pop a Lua stack root before the final byte use;
- error/debug formatting and buffer copy-on-write borrows; and
- trace compilation and assembler paths which retain raw string fields.

An unrelated long native call may delay coarse epoch reclamation but may not
block mutators or the rest of GC2. Precise hazards are required before claiming
the final performance target.

## Store-site requirements

Publication barriers must precede the actual release store or CAS. Existing
after-store patterns such as stack self-publication and VM barrier stubs require
reordering.

The audit includes:

- stack slot stores and stack-to-stack copies;
- open and closed upvalue stores;
- table array and hash value stores;
- string table-key publication;
- VM fast-path `TSET*`, `TSETM`, return, vararg, and iterator copies;
- JIT direct stores and helper-backed retry stores;
- channels and cross-TG value transfer;
- registry/global/native side-root publication; and
- FFI caches, CType names, trace roots, and error side roots.

A helper which prepares once, waits through a safepoint, and later CASes a slot
is incorrect. It must re-prepare per attempt or retain the value in an explicit
pending root through the wait.

## Resize and secondary-hash compatibility

`RESIZE` and `SWEEP` remain mutually exclusive topology-owner bits. The sweep
owner holds stable main topology through quarantine publication and the
main-unlink/re-link decision. Ordinary interners ignore `SWEEP` and continue
with exact CAS operations. Resize and secondary rehash fail fast and retry
later; they do not wait behind sweep.

If an abort can expose a `CANDIDATE` or `DEAD` edge to a later rebuild, the
rebuild carries the target's incoming `DEAD` bit exactly. A record-carrying
state may not be abandoned during resize. It must finish detach, re-link, or
remain discoverable in quarantine.

Quarantine headers themselves use RCU/SMR replacement. A quarantine lookup
pins the exact generation before following a record link.

## Failure, abort, and shutdown rules

- Record OOM restores `LIVE` and retains the main identity.
- A failed main-edge CAS keeps the published quarantine record and retries the
  moved exact edge; it never opens an absence gap.
- `CANDIDATE` may be conservatively restored on abort.
- `QACTIVE`, `QRESCUED`, and `QCLOSING` may not be discarded. Abort completes
  detach, re-links the same body, or leaves a fully discoverable quarantine
  owner.
- `QCOMMIT` never becomes the old live identity again.
- `sweep_pending` covers every interval before stable bucket or retire-list
  ownership.
- Terminal shutdown stops workers, drains pending owners, completes or cancels
  prepared records according to their body word, frees each body once, and
  frees records only after no bucket or body references them.
- Worker TG retirement transfers record pools and outstanding metadata before
  the TG's arena or registry node can be reclaimed.

## Canonical uniqueness proof sketch

Maintain these invariants:

1. **Joint-directory uniqueness:** before `QCOMMIT`, every acquirable byte
   sequence is represented by exactly one body in main, quarantine, or both.
2. **No unchecked publication:** a strong store exposes only a body for which
   prepare succeeded without an intervening quiescent point.
3. **Atomic rescue/death arbitration:** rescue and `QCOMMIT` change the same
   body word.
4. **No pre-acquisition UAF:** read epoch publication precedes the pointer load;
   native raw bytes have an explicit borrow.
5. **No early reuse:** body and record reuse occur only after directory removal
   and `E2` grace.

Before main unlink, a main lookup finds the old body. After main unlink and
before commit, the quarantine lookup finds the same body. A new insertion is
allowed only after both lookups establish logical absence. Therefore no new
body can be published while the old body is acquirable.

A publisher racing close either changes `QCLOSING` to `QRESCUED`, forcing the
same body back to main, or loses to `QCOMMIT` and must not publish the old body.
Thus no publication can make the old identity live after a replacement is
allowed.

The second grace is only a physical-lifetime proof. It is intentionally not
used as the logical-identity proof.

## Staging plan

### Stage A: lifecycle word and quarantine directory

- Add `GCstr.canon`, state accessors, record type, quarantine buckets, header
  pins, and retired-record drain.
- Make an intern main miss consult quarantine before construction/insertion.
- Keep production body reclamation disabled.
- Add deterministic tests for main-miss versus quarantine-insert ordering and
  quarantine-header replacement.

Exit criterion: no lookup can publish an equal body while an acquirable
quarantine record exists.

### Stage B: publish-before-unlink and same-body re-link

- Change TAG to `LIVE -> CANDIDATE` before `DEAD`.
- Publish records before main-edge unlink.
- Implement `QACTIVE/QRESCUED`, exact main unlink, and same-body help re-link.
- Do not implement `QCOMMIT` or physical body free yet.
- Exercise metadata OOM, moved incoming edges, abort, resize, and shutdown.

Exit criterion: arbitrarily delayed rescue between main unlink and re-link
always returns the original body and never creates an equal replacement.

### Stage C: centralized strong publication

- Route every strong string root/marker through lifecycle rescue.
- Add prepare-before-store to interpreter and C helper paths.
- Add a per-TG pending publication root for operations spanning retries or
  safepoints.
- Audit weak-edge behavior separately.
- Retain all quarantined bodies.

Exit criterion: deterministic hooks prove that a publisher at every point from
candidate tagging through `QCLOSING` either rescues or is forced to retry.

### Stage D: VM, JIT, FFI, and native acquisition epochs

- Publish VM/API read epochs before shared loads and roll them at real polls.
- Publish JIT epochs at trace entry/exit and guard direct string stores.
- Add native string-borrow tracking and prevent remote ACK from impersonating
  string quiescence.
- Audit internal raw-byte borrowers and callback nesting.
- Keep `QCOMMIT` and body free disabled.

Exit criterion: a paused interpreter instruction, trace, callback, and native
call each prevents the relevant close/reuse boundary without stopping unrelated
mutators or GC work.

### Stage E: logical commit without physical free

- Enable `QACTIVE -> QCLOSING`, `E1` acquisition grace, and the exact
  `QCLOSING -> QCOMMIT` arbitration.
- Permit replacement interning after `QCOMMIT`.
- Retain committed old bodies and records for diagnostics.
- Assert that no strong root or publication returns the committed body.

Exit criterion: race stress observes no simultaneous live equal identities and
no committed body in a strong semantic slot across interpreter, JIT, FFI, and
native cases.

### Stage F: directory unlink, second grace, and physical reuse

- RCU-unlink committed quarantine records.
- Add `E2` acquisition/native-borrow grace and terminal `FREEING` claim.
- Integrate allocator quarantine and address-reuse validation.
- Pool/batch records and benchmark miss, intern, GC, JIT, and FFI workloads.
- Run assertion, ASan, UBSan, race-stress, Wine, and Darling coverage before
  changing the production gate.

Exit criterion: string count and memory shrink before `lua_close`, old addresses
are safely reused, all canonical race tests stay green, and normal main-table
hit throughput remains close to or better than regular LuaJIT.

## Required deterministic race tests

At minimum, hooks must stop threads at these boundaries:

1. publisher loaded `LIVE`, before TAG CAS;
2. after `CANDIDATE`, before `DEAD`;
3. quarantine record bucket-published, before body Q state;
4. body `QACTIVE`, before main unlink;
5. after main unlink, before rescue;
6. `QCLOSING`, with rescue versus `QCOMMIT` CAS;
7. after `QCOMMIT`, while an equal replacement interns;
8. Q record unlinked, before `E2` completes;
9. `FREEING`, before allocator reuse;
10. resize/secondary rehash attempts in every preceding state;
11. metadata OOM and shutdown in every preceding state; and
12. JIT/native raw-byte use spanning both grace boundaries.

Tests must compare actual `GCstr *` identity, not only byte equality, and must
verify the old pointer is never stored after commit.

## Performance shape

The intended steady-state cost is:

- main intern hit: one lifecycle load only when string retirement is active,
  with no quarantine hash lookup;
- main miss: one quarantine lookup before allocation;
- ordinary string byte reads and pointer comparisons: no forwarding chase;
- strong stores outside active string close: a predictable inactive gate;
- native calls without string bytes: no exact string hazard; and
- GC retirement: batched records and bounded bucket work.

Packing the canonical state into existing header bits or a lazy arena side plane
may recover the proof-first word overhead after correctness is established. It
must not make raw custom `lua_Alloc` memory unidentifiable when arbitrary
allocators are restored later; that restoration needs an equally authoritative
allocation/lifecycle registry.

## Non-negotiable proof limits

1. **No mark-only authorization.** A mark recheck cannot replace the lifecycle
   CAS which arbitrates rescue against `QCOMMIT`.
2. **No unlink-before-reservation.** The quarantine record and body Q state are
   visible before the main edge is removed.
3. **No insertion after main-only miss.** Every main miss consults quarantine
   before publishing a new body.
4. **No after-store rescue.** Strong string preparation precedes the actual
   shared store and is repeated after any wait or safepoint.
5. **No post-load epoch entry.** The read-side epoch is published before the
   shared pointer load.
6. **No synthetic native quiescence.** Remote native handshake ACK does not
   end a raw string-byte borrow.
7. **No old-body forwarding burden on ordinary Lua.** A live reference retains
   stock pointer identity; committed old bodies are rejected or substituted at
   publication boundaries, not silently followed by every read.
8. **No record disappearance from an armed state.** Pending, bucket, body, and
   retired ownership overlap so shutdown always finds the owner.
9. **No allocator reuse before `E2`.** Header/type validation cannot repair an
   address ABA after same-type reuse.
10. **No finite guarantee for untracked raw pointers.** If “arbitrary racy” is
    extended to a C pointer held forever outside Lua roots, TG epochs, hazards,
    and the documented Lua API lifetime, finite body reclamation is impossible
    without permanent handles/tombstones or never reusing the address.

The last limit is fundamental, not an implementation inconvenience. Within
the supported Lua, VM, JIT, FFI, and API publication contracts, stages A through
F provide the required canonical identity and physical lifetime proof.

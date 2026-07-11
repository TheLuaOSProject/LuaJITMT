# Stable TG registry enumeration migration audit

Status: read-only design audit of the raw `gc2.tg_list` consumers after the
standalone `LJTGRegistrySlot` primitive landed. This note does not claim that
runtime integration exists yet.

## Non-negotiable result contract

A stable-slot walk cannot expose a bare `TGState *` and cannot reduce every
non-success result to “not found”. The minimum result vocabulary is:

```text
ITEM                 exact body borrow acquired
END_COMPLETE         every stable slot was classified
ATTACHING_GAP        canonical ATTACHING/BUSY body-publication gap
INCOMPLETE           bounded retries lost to lifecycle/lease progress
PINNED_BODY          tagged body is permanently stable, but reclaim is vetoed
PINNED_NO_BODY       pinned canonical rootless slot
CORRUPT              malformed token or token/body/tag mismatch
```

`EMPTY`, `RETIRED`, `RECLAIMING`, and `EXHAUSTED` are not borrowable items.
They may be skipped only after their exact token/body invariants are checked.
An admitted borrower which raced `DETACHING -> RETIRED` may finish because its
lease prevents `RECLAIMING`; a new borrower which observes `RETIRED` is denied.

`ATTACHING_GAP` is the one expected body mismatch: token incarnation `i` is
`ATTACHING` while the tagged body remains exactly `{NULL, i-1}`. It is rootless
only if attach ordering forbids all TG use/root publication before the body is
published. During the dual-registry migration, callers must conservatively
veto closure/progress on this result. It may become skippable for a particular
operation only after attach commit revalidates that operation's exact epoch or
generation after body publication and before its first root/use publication.

`PINNED` is not “absent”. With an exact non-null `{body, incarnation}` snapshot,
the body is permanently lifetime-stable because `PINNED` is absorbing. Root,
safepoint, and control operations may therefore need a special pinned-body
visit with no reclaim permission. Merely skipping it can lose semantic roots
or leave a running VM on stale dispatch. A pinned null canonical ATTACHING gap
is rootless. Any other null/mismatched pinned body is `CORRUPT`. In all pinned
cases, TG and GC physical reclamation remain globally disabled.

`INCOMPLETE`, `ATTACHING_GAP`, `PINNED_*`, and `CORRUPT` must never authorize:

- an owner-absent decision;
- root/mark/sweep closure;
- string/JIT/SSB quiescence;
- raw-list unlink followed by body free; or
- conversion of a `lua_State` owner id to `LJ_THREAD_GCSCAN`.

In particular, the current test at `lj_gc2.c` in `gc2_traverse_thread()`:

```c
lj_tg_find_owner(g, owner) == NULL &&
lj_state_owner_cas(th, &expect, LJ_THREAD_GCSCAN)
```

is legal only after an `END_COMPLETE` lookup reports exact absence. BUSY,
PINNED, LOST/incomplete, or corrupt enumeration must requeue the thread. Treating
any of them as NULL creates a direct stack-ownership/UAF race.

## Proposed scoped iterator

The low-level iterator should contain only stable slot state. Slots and
`next_all` remain address-stable until universe shutdown.

```c
typedef struct LJTGRegistryIter {
  global_State *g;
  LJTGRegistrySlot *next;
  uint32_t retry_budget;
  uint32_t hazards;
} LJTGRegistryIter;

typedef struct LJTGRegistryItem {
  LJTGRegistryBorrow borrow; /* Active for ITEM, centrally released. */
  TGState *tg;
  LJTGSlotSnap lifecycle;
  uint8_t pinned_body;
} LJTGRegistryItem;
```

For each stable slot, the implementation should:

1. Snapshot `next_all`; it is immutable and does not require a body lease.
2. Obtain a borrowable key and exact body borrow. Retry `LOST` only for a small
   bounded budget; expose persistent contention as `INCOMPLETE`.
3. Validate `tg->gl == g` and the TG's reverse `{slot, incarnation}` identity
   while the borrow is held. A mismatch is sticky corruption, not a skipped
   foreign body.
4. Invoke the visitor while the lease covers the complete body and subordinate
   access.
5. Release centrally before honoring visitor STOP/ERROR. A one-shot
   `lj_tgregistry_try_release()` is insufficient: `LOST` means the lease is
   still active. Retry release on `LOST` (lock-free progress), or transfer it to
   a durable deferred-release descriptor. Never clear the local handle or
   return while silently leaking an active lease.

A callback-based walk is preferable for ordinary loops because it can release
on every early return centrally. A low-level `next()` API is still needed for
lookups that transfer the winning borrow to their caller. Every such API must
make the transfer explicit.

Suggested lookup contract:

```c
typedef enum LJTGFindResult {
  LJ_TG_FIND_CORRUPT = -3,
  LJ_TG_FIND_PINNED = -2,
  LJ_TG_FIND_INCOMPLETE = -1,
  LJ_TG_FIND_ABSENT = 0, /* Valid only after END_COMPLETE. */
  LJ_TG_FIND_FOUND = 1
} LJTGFindResult;

LJTGFindResult lj_tg_find_owner_borrow(global_State *g, uint32_t tid,
  LJTGRegistryBorrow *borrow, TGState **tgp);
```

The caller owns and releases `borrow` on `FOUND`. Most boolean users should
instead use an operation-scoped callback so a raw body cannot escape.

While the old raw reclaimer can still free TG bodies, a stable borrow alone is
not protection because that reclaimer does not consult the lease word. During
migration, either keep the legacy SMR reader across the complete stable walk,
or first change physical reclaim to require both:

```text
legacy raw list unlinked + zero legacy SMR readers
and
stable RETIRED/1 -> RECLAIMING/0 success
```

No old raw-list path may free a TG after stable borrowers start relying on the
lease count.

## Raw list callsite matrix

### Safepoint (`lj_safepoint.c`)

| Function | Body/subordinate access | Migration contract |
| --- | --- | --- |
| `safepoint_clear_consumed_polls` | Reads flags, ack epoch, reqmask, poll; mutates poll/futex state | Scoped body visit through the complete clear. BUSY is attach-rootless only after attach epoch revalidation; PINNED tagged bodies still require the control visit. |
| `safepoint_rearm_fresh_stopreq_polls` | Reads flags, reqmask, `cur_L`/`thread_L`; mutates poll | Same scoped control visit. Do not let a pinned live body resume without STOPREQ. |
| `safepoint_signal_late` | Reads lifecycle/ack, increments global pending, publishes reqmask/poll, wakes futex, calls `lj_safepoint_retire_dead_tg` | Borrow must span signaling and dead-TG pending reconciliation. ATTACHING with a body is included. A canonical gap is safe only when attach catches the same/newer handshake before roots/use. |
| `safepoint_ack_native` | Reads `in_native`/`jit_base`, then calls `safepoint_ack_tg` and `lj_safepoint_apply_tg` | Borrow spans the entire ACK/apply. The lease is lifetime only; native-stop/owner protocol remains the mutation authority for stack, SSB, allocator and dispatch fields. |
| `safepoint_trace_tg_active` | Reads tid and `jit_base` | Predicate walk. Any incomplete/uninspectable root-bearing slot returns “trace active”, never quiescent. |

The raw `self` values in the same file (`lj_thr_get_tg*`) must be backed by the
long-lived TLS borrow. The tactical `lj_gc2_smr_read_enter()` around the final
two list walks remains a legacy extra veto until no path uses `next_tg`.

### GC2 (`lj_gc2.c`)

| Function | Class | Migration contract |
| --- | --- | --- |
| `gc2_small_arena_registry_clear_refs` | Terminal subordinate mutation | Shutdown-only. Visit each body while borrowed or while terminal reclaimer owns it; clear `tg->alloc.registry` before slot clear/free. |
| `gc2_worker_tg_registered` | Pointer-membership predicate | Replace with the worker TG's exact stable key/lifecycle, not a body-pointer scan. The worker-retire list must itself own a key/lease. |
| `lj_gc2_terminal_reclaim_tgs` registry-shape loop | Terminal invariant | Inspect stable token states plus the legacy shape. Ordinary borrowing cannot enumerate already-RETIRED bodies; terminal code must require every non-main incarnation EMPTY/EXHAUSTED (or fail shutdown). |
| `gc2_reset_alloc_trigger` | Remote atomic/accounting mutation | Scoped borrow through `lj_gc2_flush_alloc`. Incomplete walk must leave cycle start conservative and retry/veto closure. |
| `gc2_tg_for_registered_mem` (both loops) | **P0 raw body return** | Replace with an operation callback or an owner-borrow result. Never return a `TGState *` after releasing the iterator borrow. |
| `gc2_tg_for_mem` | **P0 raw body return** | Same; current owner-id fast path also needs a TLS/main borrow or an exact owner borrow. |
| `gc2_clear_marks_all` | Shared bitmap/subordinate mutation | Borrow spans `gc2_clear_marks`; worker/phase protocol remains mutation authority. Incomplete enumeration must force NO_RECLAIM rather than start a partially reset cycle. |
| `gc2_tg_list_contains` | Pointer-membership predicate | Replace with exact key state. It currently decides whether a TLS body needs attach; pointer equality is not incarnation identity. |
| `lj_gc2_sweep_needs_prepare` | Subordinate read predicate | Scoped borrow across `prepare_epoch`; incomplete means “needs prepare”. |
| `lj_gc2_sweep_needs_restore` | Subordinate list read predicate | Scoped borrow across `prepare_epoch` and every `needsweep[]` read; incomplete means “needs restore”. |
| `lj_gc2_sweep_pending` | Subordinate list/hugetab predicate | Scoped borrow; incomplete/pinned means pending. |
| `lj_gc2_sweep_live_aggregate` | Subordinate arena/hugetab traversal | Borrow spans every arena list and hugetab aggregate. Do not return a list head beyond the visit. Incomplete aggregation cannot update pacing as an exact completed-cycle estimate. |
| `gc2_obj_mem_live_kind` (two loops) | Memory membership predicate | Scoped body/hugetab visit. Return INVALID/unknown conservatively on incomplete; do not touch a candidate arena header based on an incomplete owner scan. |
| `gc2_huge_exact_traversable` | Hugetab predicate | Scoped borrow. Incomplete means not proven traversable, but must not be interpreted as permission to free. |
| `gc2_queue_small_cell_live` | Arena-list membership predicate | Scoped borrow. The shared small-arena registry should replace this fallback in steady state. |
| `gc2_discard_active_ssb_` | Terminal owner-private mutation | Terminal-only after mutators/workers join. Borrow or terminal reclaim ownership must cover active embedded SSB node and cursor mutation. |
| `lj_gc2_ssb_empty` | Subordinate cursor predicate | Scoped borrow. Any incomplete/pinned unvisited body means non-empty. Main/current special cases require their universe/TLS leases. |
| `gc2_stack_mem_valid` | Subordinate arena-list predicate | Scoped borrow through all list reads. Do not export an arena/list pointer; incomplete means “not proven valid”. |
| `gc2_worker_sweep_progress` | Physical/owner-list mutation | Borrow must span the complete `lj_gc2_sweep_owner_progress` call, including arena quarantine, hugetab mutation, destructors and nested handshakes. The worker token remains the mutation authority. Any iterator hazard stops the batch. |
| `lj_gc2_paranoia_root_diff` | Test/paranoia subordinate traversal | Scoped borrow; report a hazard rather than silently omitting an incarnation. |

The main-TG direct accesses may remain temporarily under the live universe
owner, but the main TG should still have a stable slot so enumeration does not
need a “saw main” correctness exception.

### GC (`lj_gc.c`)

| Function | Class | Migration contract |
| --- | --- | --- |
| `lj_gc_sweep_gc2_all_arena_bodies` | Physical arena mutation | Borrow spans both arena-list walks and every destructor/body sweep. Require the existing worker/sweep authority in addition to lifetime; an incomplete registry walk aborts the batch. |
| `lj_gc_flush_root_pending` | Lock-free subordinate-chain mutation | Borrow spans both pending-chain exchanges and root-spine prepend. On BUSY/incomplete, republish the global pending hint and report incomplete; clearing the hint and skipping a TG can lose fresh objects. ATTACHING bodies must be included before their first pending-root publication. |

The current main/self fallback exists because attach can publish roots before
raw-list insertion. Stable attach ordering must eliminate that gap. Until then,
the TLS-current fallback is valid only under its long-lived TLS borrow.

### State shutdown (`lj_state.c`)

| Function | Class | Migration contract |
| --- | --- | --- |
| `close_state_arena_free_noinsert` | Terminal owner-private allocator mutation | Run only after all mutators/workers join. Borrow each still-borrowable body or perform it while terminal reclaimer owns the exact body. A RETIRED body cannot be recovered through an ordinary iterator, so this mutation must precede RETIRED or move into terminal finalization. |

### Strings (`lj_str.c`)

| Function | Class | Migration contract |
| --- | --- | --- |
| `strtab_active_on_hdr` | Independent-retire guard | Borrow spans depth/header reads. Incomplete or pinned-uninspectable means active. |
| `strtab_active_on_hdr_before` | Epoch-qualified retire guard | Same, including `strtab_active_epoch`. Do not reclaim an old header on incomplete enumeration. |
| `strq_active_on_hdr` | Canonical-quarantine guard | Same for `strq_active_*`; incomplete means active. |

The old main/self fallback is another attach-visibility workaround. It should
disappear only after stable ATTACHING body publication precedes any string read
pin and TLS holds its ordinary long-lived borrow.

### Profiler (`lj_profile.c`)

| Function | Class | Migration contract |
| --- | --- | --- |
| `profile_tg_drop_all` | Remote control mutation | Borrow spans hookmask/sample/vmstate updates. A tagged PINNED body still needs profile-stop control. ATTACHING gaps are skippable only if attach adopts current profile state before VM entry and revalidates it. |

### Threading library (`lib_threading.c`)

| Function | Class | Migration contract |
| --- | --- | --- |
| `threading_tg_is_registered` | **P0 membership-to-free decision** | Delete this pointer-membership test. `threading_thread___gc` currently uses false to call `lj_tg_fini_thread` and `lj_mem_freet` directly. `LJThread::tg` must become an exact key/lifecycle owner (or hold a documented lease), and only stable `RETIRED/1 -> RECLAIMING/0` ownership may finalize/free. BUSY/PINNED/incomplete always retains the body. |

`LJThread::tg`, `lua_State::tg_hint`, GC2 `worker_tg[]`, embedded SSB owner
pointers, allocator owner-TG pointers, and JIT owner caches are all raw-holder
dependencies even when they are not list loops. Each needs either a long-lived
keyed borrow, an exact key that is borrowed at use, or removal.

### TG lifecycle (`lj_tg.c`)

These are the list owner/reclaimer paths and cannot be converted as ordinary
read visitors:

| Function | Class | Migration contract |
| --- | --- | --- |
| `lj_tg_attach` duplicate scan/CAS prepend | Lifecycle writer | Claim/publish/link the stable slot before any root-bearing field. Store the key in the TG. Raw-list insertion remains dual-registry compatibility, not lifetime publication. |
| `tg_terminal_pending_roots_empty` | Terminal predicate | Stable token/body-state audit plus legacy list; incomplete fails terminal reclaim. |
| `tg_lua_storage_owner_follows` | Terminal raw-list topology | Stable slot order is allocation/reuse order, not legacy newest-first attach order. Keep this terminal legacy check until Lua TG storage ownership no longer depends on list order. |
| `tg_reclaim_dead` raw unlink/free loop | **P0 physical lifecycle owner** | Stop freeing from raw traversal. First unlink under legacy writer/zero-SMR proof, then RETIRE stable key, then exact stable reclaim after all borrows/TLS leases drain, then finalize/free and tagged-clear. |
| `lj_tg_find_owner` | **P0 raw body return** | Replace by borrow-return/operation-scoped APIs with exact COMPLETE_ABSENT. |
| `lj_tg_any_jit_active` | Global quiescence predicate | Scoped walk; incomplete/PINNED means JIT active. |
| `lj_gc2_terminal_reclaim_tgs`/terminal shape checks | Terminal invariant | Require stable slots quiescent and no active TLS borrows before GG destruction. |

The worker-retired raw list is a second registry of TG bodies. Its link owner
must retain an exact key or lease; “not in `gc2.tg_list`” is not sufficient
permission to free once stable enumeration is active.

## Raw-return and subordinate-pointer matrix

The following must migrate atomically with `lj_tg_find_owner`; changing only
its internal loop is unsafe.

| Current function/callsite | Escaping value/use | Required replacement |
| --- | --- | --- |
| `lj_dispatch.c:dispatch_state_mode` / `lj_dispatch_update` | Returns recorder TG through `tgp`, later validates `cur_L` and writes `tg->dispatch` | Transfer a borrow through the entire validation/update/retry iteration, releasing on every retry/return; or perform the recorder dispatch overlay in a scoped owner callback. |
| `lj_thr.c:state_stack_dirty` | Increments owner TG dirty epoch | Operation-scoped find-and-increment. Incomplete is conservative dirty/needs-rescan, not absent. |
| `lj_trace.c:lj_jit_owner_tg_l` / `jit_token_tid_l` | Returns TG, then reads tid | Replace `tg_hint` with exact key and return a copied tid only after scoped validation, or return a borrow to caller. |
| `lj_trace.c:trace_arena_allocd_for_ptr` | Returns `&tg->allocd`; caller later reads hugetab | Return an arena-owner lease object containing borrow + `allocd`, held through the full lookup/operation. |
| `lj_gc.c:gc_arena_allocd_for_ptr` | Returns `&tg->allocd`; `lj_mem_realloc`/`lj_mem_free` call allocator afterward | Same arena-owner lease, held through `lj_arena_allocf`. This is a direct subordinate-pointer UAF if the borrow ends in the helper. |
| `lj_gc.c:gc2_size_fits_mem` | Reads owner hugetab | Operation-scoped predicate. |
| `lj_gc.c:gc2_sweep_obj_old_generation` | Reads owner hugetab | Operation-scoped predicate under sweep owner. |
| `lj_gc.c:gc2_sweep_detached_obj` | Mutates owner hugetab retirement ticket | Operation-scoped borrow through the exact hugetab mutation. |
| `lj_gc2.c:gc2_tg_for_registered_mem` / `gc2_tg_for_mem` | Returns TG used later for flags, hugetab, marking and allocator ownership | Return a body borrow/arena-owner lease, or fold each complete operation into the lookup callback. |
| `lj_gc2.c:gc2_mark_base_traversable` | Uses returned owner hugetab | Operation-scoped lookup. |
| `lj_gc2.c:gc2_markmem_status`, `gc2_markmem_registered_scoped_status`, `lj_gc2_ismarkedmem` | Marks/queries owner huge metadata after lookup | Hold owner borrow through hugetab mark/query. Arena rescue scope does not pin the TG body that owns the hugetab. |
| `lj_gc2.c:gc2_thread_jit_base` | Returns `jit_base` subordinate pointer after owner lookup | Hold owner borrow through snapshot validation/frame use, or combine with the existing state/stack claim. |
| `gc2_thread_is_current`, `gc2_thread_is_jit_current`, `gc2_thread_is_native_current`, `gc2_thread_is_remote_current` | Owner predicates | Operation-scoped lookups; incomplete returns conservative remote/native/JIT-active, never “not current”. |
| `gc2_thread_owner_dirty` | Optionally returns TG and separately returns epoch | Remove the TG out-parameter; copy epoch under borrow. Incomplete must force dirty mismatch/rescan. |
| `gc2_thread_has_live_owner` | Boolean owner existence | Tri-state lookup; incomplete means owner may be live. |
| `gc2_traverse_thread` stale-owner takeover | NULL authorizes owner CAS to `LJ_THREAD_GCSCAN` | Require exact `END_COMPLETE/ABSENT`; every hazard requeues. This is the highest-priority semantic conversion. |

The 32-bit `tid` itself wraps in `lj_thr_newid()`. Stable body incarnation
prevents pointer ABA but not owner-id ABA. Eventually published owner references
(`lua_State` owner, arena owner, JIT owner) need an exact stable key/incarnation
or a separate non-wrapping owner generation; scanning by tid alone cannot prove
identity forever.

## ATTACHING and detach ordering required by enumeration

Attach must use this order before stable scans can treat a canonical gap as
rootless:

1. Keep the Lua universe alive for the entry operation.
2. Claim slot `ATTACHING` with owner lease.
3. Initialize the TG privately, publish tagged body/global association, and
   link a new immutable slot (a reused slot is already linked).
4. Install TLS `{slot, incarnation, body}` with its own long-lived borrow.
5. Publish the attach/root descriptor and sample the exact activation gate.
6. Apply/revalidate the current safepoint epoch, phase mirrors, dispatch,
   STOPREQ/shutdown state, and profile state.
7. Publish `cur_L`, `thread_L`, `thread_ud`, `tg_hint`, pending-root and SSB
   producer visibility. Dual-link the legacy list before any legacy-only reader
   could otherwise miss them.
8. Revalidate activation and handshake generations, publish LIVE, revalidate,
   then permit VM/JIT/native/string-table entry.

Detach must enter DETACHING before clearing roots, flush/account subordinate
state, clear all raw root publications, clear `tg_hint` and every long-lived raw
holder, publish TLS cached body NULL, release the TLS borrow, complete legacy
pending ACK/list retirement, and only then publish RETIRED after the final
owner-side TG access. RETIRED closes new enumeration admission.

## Safe migration order while the legacy list remains an extra veto

1. Add per-universe stable spine/head, reverse key in every TG, stable main and
   worker slots, and exact status telemetry. Do not change free behavior yet.
2. Make the old physical reclaimer honor stable lifecycle/leases, or retain one
   legacy SMR reader across every stable walk. Without this, the first stable
   iterator can still race old `free(tg)`.
3. Install long-lived keyed TLS and convert `LJThread::tg`, `tg_hint`, worker-TG
   and other persistent raw holders. Clear cached body before releasing a
   binding.
4. Reorder attach/detach as above. ATTACHING body publication must precede any
   root producer; RETIRED must follow the last use.
5. Convert safepoint signaling/ACK and GC/pending-root enumeration first. These
   define attach visibility and root coverage.
6. Convert all borrow-return/operation-scoped owner lookups and their transitive
   allocator/JIT/GC callers as one unit. Forbid raw `TGState *` return APIs.
7. Convert string/JIT/SSB quiescence predicates; all incomplete results veto.
8. Convert sweep/arena/hugetab and memory-validation loops, keeping worker,
   arena rescue, SMR and activation predicates as additional authorities.
9. Convert profile/control loops and terminal-only mutation/reclaim paths.
10. Run stable and legacy enumeration in shadow mode. A stable-only root-bearing
    body, legacy-only live body, wrong reverse key/global, BUSY close decision,
    or count mismatch sets the global no-reclaim veto. Do not apply mutating
    callbacks twice.
11. Remove legacy `next_tg`/SMR only after every raw holder and physical free
    site has migrated and cross-platform churn tests remain clean.

## Required tests

- Pause a reused slot at canonical ATTACHING/BUSY. Enumeration must not
  dereference a body or pin it as corruption. A close/quiescence predicate must
  veto until attach generation revalidation is proven.
- Pause after ATTACHING body publication and before LIVE. Safepoint/root scans
  must borrow and visit it; attach must receive a concurrently published epoch.
- Borrow LIVE, transition through DETACHING/RETIRED, prove new borrows are
  denied, the admitted borrower can finish subordinate reads, reclaim stays
  BUSY, and release enables exact reclaim.
- Force `try_release` CAS loss with concurrent borrow/release/lifecycle traffic;
  every early STOP/ERROR path must return the lease count to the owner baseline.
- Exercise PINNED with a valid tagged body, pinned canonical rootless gap, and
  malformed tagged body. Root/control behavior and permanent reclaim veto must
  differ exactly as specified.
- Churn attach/detach/reuse while safepoint signaling, pending-root flush,
  `lj_tg_any_jit_active`, string active guards, SSB emptiness, GC root scans and
  owner lookups run concurrently under ASAN/UBSAN/TSAN.
- Pause an owner lookup after FOUND while another thread detaches/retires. Use
  the owner hugetab/allocator/dispatch under the transferred borrow, then prove
  physical reclaim occurs only after release.
- Return BUSY/PINNED/incomplete for a state owner lookup and prove
  `gc2_traverse_thread` never CASes that owner to `LJ_THREAD_GCSCAN`.
- Shadow-registry mismatch tests: stable-only ATTACHING body, legacy-only live
  body, wrong reverse key, wrong `global_State`, and duplicate tid all veto.
- TLS tests: binding install owns one lease; cached body clears before release;
  stale key cannot resolve a reused slot; deliberately retained TLS keeps
  RETIRED reclaim BUSY.
- `LJThread` userdata finalization must never directly free a body based on a
  membership scan. BUSY/incomplete keeps ownership; only exact stable reclaim
  finalizes once.
- Terminal tests require joined threads/workers, no active TLS/body borrows,
  stable non-main slots EMPTY/EXHAUSTED, and legacy shape agreement. PINNED or
  ATTACHING at shutdown must fail closed rather than free under a stale holder.
- Repeat GCC/Clang sanitizer stress, MinGW/Wine, and Darwin/Darling artifact and
  runtime tests, including inline CX16/no accidental `libatomic` dependency.

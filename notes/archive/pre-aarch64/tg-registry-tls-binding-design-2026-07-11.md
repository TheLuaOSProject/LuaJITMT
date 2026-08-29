# Stable TG registry TLS binding design and caller audit

Date: 2026-07-11

Status: design/audit only. The three-index Windows representation in this note
is superseded by the simpler tagged-word implementation documented in
`tg-registry-tls-tagged-binding-2026-07-12.md`. The lifecycle ordering,
caller inventory, universe-lifetime requirement, and remaining-holder audit
remain applicable. This note does not make the stable registry positive
reclamation authority and does not claim that the remaining raw TG holders are
safe.

## Result

Raw `TGState *` TLS must be replaced by a TLS binding which owns one ordinary,
long-lived `LJTGRegistryBorrow`. The hot getter must not borrow, validate a CX16
word, scan the registry, allocate, or wait. It returns only the body cached by
that already-held borrow.

The binding linearization rules are:

- install: acquire the exact body borrow first; publish the cached body last;
- replace: acquire and publish the new binding before releasing the old borrow;
- clear: publish cached body `NULL` first, then release or move out the borrow;
- retirement: do not publish `RETIRED` until `tg_hint`, state ownership, TLS,
  and every final owner-side TG use have ended.

These rules are required for the POSIX `SIGPROF` handler. A signal which arrives
before the `NULL` store finishes while the interrupted thread still owns the
borrow. A signal which arrives afterward observes `NULL`. The signal path never
touches a registry token.

## Current defects this migration closes

The current `lj_thr_set_tg(TGState *)` is a void raw-pointer store. It cannot
report a Windows TLS failure, does not acquire a body lease, and lets a raw
cache survive independently of the body lifetime authority. The Windows getter
also runs `InitOnceExecuteOnce` on the hot path.

There are ordering defects around it:

- Main-TG bootstrap installs TLS before `tg_init_common()` has initialized the
  body's `gl` and other remotely read fields.
- Spawned Lua workers, foreign callback carriers, and GC workers install raw
  TLS before `lj_tg_attach()` has made the TG globally discoverable.
- Foreign detach currently lets `lj_tg_detach()` clear TLS and publish the
  legacy dead boundary before the outer caller clears `L->tg_hint` and releases
  `lua_State` ownership.
- Pointer equality in main shutdown is not an exact binding identity test; the
  stable identity is `{slot, incarnation}`.

The stable binding fixes only the current OS thread's cache. It does not make
`lua_State::tg_hint`, `LJThread::tg`, `worker_tg[]`, SSB owner pointers, arena
owner pointers, or legacy TG-list walks lifetime-safe.

## Exact logical representation

The platform-independent logical state is:

```c
typedef struct LJThrTGBinding {
  TGState *hot_body;             /* Atomic hot getter word. */
  LJTGRegistryBorrow hold;       /* Owner-private linear lease handle. */
} LJThrTGBinding;
```

`hot_body != NULL` implies all of the following:

1. `hold.active != 0`;
2. `hold.body == hot_body`;
3. `hold.key` names the exact published body incarnation;
4. that ordinary borrow has not been released;
5. the body `gl` association and its embedded registry key were initialized
   before body publication and are immutable for the incarnation.

The converse must also hold in steady state. Temporary install/clear code may
have an active local borrow while `hot_body == NULL`, but it may not return to
the VM in that state without either completing installation or returning the
linear handle to its caller.

`TGState` needs an immutable-per-body key, preferably appended after assembly-
addressed fields:

```c
LJTGRegistryKey registry_key;
```

This is useful for diagnostics and reverse association checks, but it does not
replace the per-binding borrow handle. Multiple OS threads can accidentally or
deliberately bind the same TG, and each binding must own a distinct lease.

## Handle-moving API

Registry acquisition and TLS publication should be separate operations. That
keeps slot result handling out of platform TLS code and avoids silently losing
a linear borrow on an OS failure.

```c
typedef enum LJThrTGResult {
  LJ_THR_TG_OK = 1,
  LJ_THR_TG_EXPECT_MISMATCH = 0,
  LJ_THR_TG_INVALID = -1,
  LJ_THR_TG_TLS_FAILURE = -2,
  LJ_THR_TG_CORRUPT = -3
} LJThrTGResult;

LJ_FUNC int lj_thr_tg_tls_init(void);

/* Require an empty cache. Success consumes new_hold. */
LJ_FUNC LJThrTGResult
lj_thr_tg_install(LJTGRegistryBorrow *new_hold);

/* Require current key == expected_old. Success consumes new_hold and moves the
** old active handle into old_hold; the caller releases old_hold exactly once.
*/
LJ_FUNC LJThrTGResult
lj_thr_tg_swap(const LJTGRegistryKey *expected_old,
               LJTGRegistryBorrow *new_hold,
               LJTGRegistryBorrow *old_hold);

/* Publish the hot NULL edge and move the active handle into old_hold. */
LJ_FUNC LJThrTGResult
lj_thr_tg_clear(const LJTGRegistryKey *expected_old,
                LJTGRegistryBorrow *old_hold);

/* Slow owner/control-plane identity query; never used by the signal path. */
LJ_FUNC int lj_thr_tg_current_key(LJTGRegistryKey *key);

LJ_FUNC TGState *lj_thr_get_tg(void);
LJ_FUNCA TGState *lj_thr_get_tg_fallback(global_State *g);
```

On success, install/swap zero `new_hold` with
`lj_tgregistry_borrow_init()`. Swap/clear require an initially inactive
`old_hold` and populate it exactly once. On failure, ownership is unchanged and
the caller still owns every input handle, except for an explicitly reported
fail-closed deactivation described in the Windows section.

Production runtime paths only need empty-to-installed and installed-to-empty.
`swap` is useful for deterministic tests and carefully scoped control code, but
must not be used to change universes while a Lua state, native frame, callback,
or JIT frame still names the old TG.

The raw void setter should have no production callers. If its symbol is kept
for internal binary compatibility, it should be a checked compatibility wrapper
which derives the body's exact key, acquires/moves a borrow, and fail-stops on an
unreportable error. It must never remain a raw TLS write.

## POSIX representation and ordering

POSIX can use static compiler TLS with no allocation:

```c
static LJ_TLS LJThrTGBinding lj_tls_tg;
```

Only `hot_body` is accessed asynchronously. It is read with an acquire pointer
load and written with release pointer stores. The remaining fields are private
to the interrupted OS thread; a signal handler never reads them.

Install is:

1. require `new_hold->active` and a non-null exact body;
2. require current `hot_body == NULL`;
3. move the new key/body/active metadata into `lj_tls_tg.hold`;
4. release-store `new_hold->body` to `hot_body`;
5. invalidate `new_hold`.

Swap is:

1. snapshot and validate the old exact handle against `expected_old`;
2. validate the already-acquired new handle;
3. move old metadata into `old_hold`;
4. install new metadata in TLS;
5. release-store the new body to `hot_body`;
6. invalidate `new_hold`;
7. let the caller release `old_hold` after its last old-body use.

Both leases exist at the body-store edge. A signal therefore sees either the
old protected body or the new protected body.

Clear is:

1. validate the current tuple and exact expected key;
2. release-store `NULL` to `hot_body`;
3. move TLS metadata into `old_hold` and invalidate the TLS copy;
4. perform any final owner-side use through `old_hold.body`;
5. release `old_hold`, retrying ordinary `LJ_TGSLOT_LOST` CAS races.

On x86-64, the later locked release CAS cannot pass the earlier hot-body store.
Keeping both as compiler atomic operations also prevents compiler reordering.

`lj_thr_get_tg()` is exactly one TLS-relative acquire load. It must remain
async-signal-safe and cannot call `lj_tgregistry_try_body_snapshot()`.

## Windows indices, initialization, and failures

Keep the existing dynamic Windows TLS compatibility, but use three process-wide
indices so the hot getter still performs exactly one OS TLS lookup:

```c
static DWORD lj_tls_tg_body_key = TLS_OUT_OF_INDEXES;
static DWORD lj_tls_tg_slot_key = TLS_OUT_OF_INDEXES;
static DWORD lj_tls_tg_inc_key  = TLS_OUT_OF_INDEXES;
static INIT_ONCE lj_tls_tg_once = INIT_ONCE_STATIC_INIT;
```

The body index contains `TGState *`, the slot index contains
`LJTGRegistrySlot *`, and the incarnation index contains the exact `uint64_t`
encoded through `ULONG_PTR`. This project is x86-64-only for this phase, so the
encoding is lossless. A non-null body is the active marker.

Do not allocate a per-thread heap cache and do not use an FLS destructor. An
FLS destructor cannot safely detach a TG or release a universe lifetime token
after arbitrary user thread teardown. An unclean thread exit must fail closed
by retaining a lease, not manufacture detach completion.

The `INIT_ONCE` callback allocates all three indices. If allocation `n` fails,
it `TlsFree()`s every earlier index from that same attempt, restores all three
globals to `TLS_OUT_OF_INDEXES`, and returns `FALSE`. No partially initialized
key set becomes visible.

`lj_thr_tg_tls_init()` calls `InitOnceExecuteOnce()` explicitly from
`lua_newstate()` before main-TG body/TLS publication and returns failure to
`lua_newstate()`. A failed callback leaves one-time initialization retryable by
a later call. The hot getter does not call `InitOnceExecuteOnce`; before a
successful global TLS init it simply returns `NULL`.

The indices are process/plugin lifetime resources. They must not be freed by an
individual Lua universe because other universes and threads share them. Process
or DLL teardown lets the OS release them after all Lua entry is impossible.

### Windows install

With an empty body key:

1. validate `new_hold`;
2. `TlsSetValue(slot_key, new slot)`;
3. `TlsSetValue(inc_key, exact incarnation)`;
4. `TlsSetValue(body_key, new body)` -- the publication LP;
5. invalidate `new_hold`.

A failure before step 4 leaves the body key null. Clear any metadata written by
that attempt and return `LJ_THR_TG_TLS_FAILURE`; the caller still owns and must
release `new_hold`. A step-4 failure also leaves the body key null and has the
same result. No TG root or `tg_hint` may be published after this return.

### Windows swap

First reconstruct and validate the old active handle from all three indices and
copy it to `old_hold`. Then write the new slot and incarnation, and write the
new body last. Only after the body write succeeds is `new_hold` consumed.

If a metadata/body write fails before the body LP, restore the old slot and
incarnation while the old body remains published. If restoration itself fails,
the TLS tuple is not allowed to return to VM code: try to clear the body key,
return the exact old handle to `old_hold`, retain `new_hold` in the caller, and
report `LJ_THR_TG_TLS_FAILURE`. If even the body clear fails, this is a broken
process-wide TLS invariant, not a normal racy result; pin both named bodies and
fail-stop. Silently losing either lease would permit a UAF.

Runtime attach/detach does not require swap, so this exceptional path is not on
a normal VM lifecycle.

### Windows clear

Reconstruct and validate the exact old tuple first. Clear `body_key` before
touching metadata. If that write fails, leave the binding and handle ownership
unchanged and return `LJ_THR_TG_TLS_FAILURE`; the caller must not retire/free the
TG. Once body is null, move the old handle to `old_hold`. Clearing the slot/inc
indices is cleanup rather than a lifetime LP; a failure is reported, but stale
metadata is ignored while body is null and the next successful install
overwrites it.

`TlsGetValue()` can alter Win32 `LastError`. Existing FFI callback entry/leave
code already saves and restores both `errno` and `LastError` around TLS lookup,
auto-attach, and auto-detach. That preservation must remain. The Windows hot
getter must still contain only the body-key lookup; adding two metadata lookups
would regress every `G2TG()` use.

## Registry preparation and attach order

Every TG body must have a stable key before TLS can borrow it. A new/reused
attachment transaction is:

1. Hold the existing universe entry token (`mt_entering`, worker-controller
   ownership, or main bootstrap ownership).
2. Claim an external slot: `EMPTY -> ATTACHING`, installing the owner lease.
3. Initialize the complete TG body privately, including `gl`, `tid`, immutable
   `registry_key`, allocators, dispatch, and nil root fields.
4. Exact-publish `{body, incarnation}`. For a new slot, initialize `next_all`
   and release-CAS it onto the immutable registry spine. A reused slot is
   already linked.
5. Acquire an ordinary `LJTGRegistryBorrow` for the exact key. Retry only
   ordinary `LJ_TGSLOT_LOST`; do not wait on a lifecycle denial.
6. Install that borrow in empty TLS. A TLS failure aborts before any root
   publication.
7. Publish an attach/root descriptor and reconcile the typed GC activation.
8. Publish `cur_L`, `thread_L`, `thread_ud`, `tg_hint`, and phase mirrors.
9. Revalidate activation, exact-publish `ATTACHING -> LIVE`, revalidate again,
   and only then enter VM/JIT/native execution.

The registry spine must be linked before handshake catch-up. A safepoint/root
enumerator borrows linked ATTACHING bodies after body publication. A reused
slot's canonical body-publication gap returns bounded `BUSY`; the activation
descriptor handles a collector which crossed that gap.

On any failure after slot claim:

- clear every root publication already made;
- clear TLS with the exact expected key and release the returned borrow;
- exact-abort `ATTACHING -> RETIRED`;
- leave physical destruction to keyed reclaim;
- release the universe entry token last.

The existing `lj_tg_init_thread(..., L, ...)` publishes `L->tg_hint` too early
for this order. Preparation should accept the intended L separately or initialize
with root fields nil; spawn stack rehoming must take an explicit TG instead of
using a prematurely published hint.

### Main TG

Main bootstrap has no concurrent scanner, but it should use the same body/key
invariants. Fully initialize and publish its body before optionally installing a
TLS borrow. If the OS thread already has a binding for another Lua universe, do
not replace it; `G2TG(g)` may fall back to `g->main_tg` under the caller's valid
universe lifetime.

Main close clears TLS only when the current exact key equals the main key.
Pointer equality alone is insufficient. Release that borrow before destroying
main-TG subordinate storage.

## Required detach state machine

The TG owner starts with a LIVE owner lease and one TLS borrow. The exact order
for a spawned mutator, foreign callback carrier, or GC worker is:

```text
LIVE
  | exact registry CAS (close lifecycle publication intent)
  v
DETACHING                 still borrowable
  | abort recorder / disown callbacks as applicable
  | acknowledge requests; flush SSB, roots, accounting, strings, tmpbuf
  | clear cur_L/thread_L/thread_ud/jit_base/ffi_call_func
  | clear every lua_State.tg_hint that names this TG
  | release lua_State ownership while TLS borrow still protects the TG
  | clear exact TLS binding -> move borrow to local handle
  | perform final owner-side use through the local handle
  | release local TLS borrow (retry LOST)
  | publish legacy DEAD/list counters as compatibility vetoes
  | exact registry CAS (close new borrows)
  v
RETIRED                   owner lease only when ordinary borrows have drained
  | exact RETIRED/1 -> RECLAIMING/0
  v
RECLAIMING -> body clear -> EMPTY
```

The `LIVE -> DETACHING` edge must occur before the first remotely visible root
clear. `DETACHING -> RETIRED` must be the last TG-lifecycle edge, after state
release and final local use. Whether the TLS borrow release wins just before or
just after the RETIRED CAS is safe in the primitive, but doing it before RETIRED
normally leaves an immediately reclaimable owner-only count and makes the
ordering easier to audit.

`lj_threading_detach()` therefore cannot call a monolithic
`lj_tg_detach()` which completes RETIRED before it clears `L->tg_hint` and
`thr_owner`. Split detach into begin/finish, or pass the state handoff into the
TG detach routine. `state_stack_dirty()` should eventually receive the already
borrowed TG/key directly rather than rediscovering it through a wrapping owner
ID and raw list walk.

If TLS clear fails on Windows, do not publish RETIRED and do not free the body.
The ordinary recoverable outcome is a failed attach/detach operation with the
body retained. An OS thread which must exit needs a controller-owned exact
handoff record so the joined-thread controller can account for the abandoned
binding once; otherwise pin/leak is safer than guessing a release.

## Explicit caller inventory

Line numbers below are those at the time of this audit and may move while the
adjacent registry work lands.

### Raw setters: all must become handle-moving lifecycle calls

| Current caller | Required migration |
| --- | --- |
| `lj_tg_init()` main bootstrap | Publish main body/key first; borrow+install only if TLS is empty. |
| `threading_worker_cp()` | Child installs a borrow for the parent-prepared ATTACHING body before root publication. Propagate failure through worker startup. |
| `threading_worker_cleanup()` | Exact clear returning a handle; finish state handoff; release handle; then RETIRED. |
| `threading_attach_cp()` | Foreign attach borrow+install before `tg_hint`/TG root stores. Return attach failure. |
| `threading_attach_cleanup()` | Exact clear/release on every protected failure path. |
| `lj_threading_detach()` | Use the split DETACHING state machine; remove its duplicate blind raw clear. |
| `gc2_worker_main()` entry | Install the controller-prepared ATTACHING key before `lj_native_enter()` and registry/root publication. Signal startup failure. |
| `gc2_worker_main()` exit | Exact clear/release before exited publication and RETIRED. |
| `close_state()` | Clear only the exact main key; release before main body destruction. |
| `lj_tg_detach()` internal clear | Remove the blind raw setter; clear must be coordinated with state ownership and return the borrow. |
| Test-only TG switching | Use explicit borrow/install/swap/clear helpers. Do not retain a production raw escape hatch. |

### Direct hot getters: no per-call borrow, but binding must stay installed

| Current caller group | Use after migration |
| --- | --- |
| `lj_ccallback_prepare`, `lj_ccallback_unwind`, `lj_ccallback_enter`, `lj_ccallback_leave` | Signal/error-transparent hot body. Auto-attach owns one binding; nested callbacks reuse it. Do not retain the pointer after auto-detach. |
| `profile_trigger` (`SIGPROF`) | Hot-body-only acquire load. This is the path which forbids lazy init, token validation, allocation, retry, or metadata-key lookup. |
| `arena_allocf_free` | Owner-local comparison only. The arena's raw `owner_tg` is a separate lifetime migration. |
| `lj_gc_flush_root_pending`, pending-root link helpers | Current-TLS body exception remains leased; registry-list enumeration must independently borrow each remote body. |
| string active-header/quarantine checks and `str_body_retire_new` | Current body is leased. Legacy list peers and allocator ownership remain separate hazards. |
| table structural owner/current-concurrency helpers | Current body/tid only; do not store the returned pointer. |
| GC2 finalizer/current-worker/fixpoint helpers | Current body only for the duration of the call. Control-plane worker pointers need keys. |
| safepoint contender self lookup | Current body remains leased through the handshake. Every other enumerated TG needs a scoped registry borrow. |
| `lj_tg_derive_prng` | Snapshot parent PRNG while its binding is installed; do not retain parent TG. |
| `lj_threading_shutdown`, attach-current checks, detach lookup | Use hot body for observation, but use exact current-key query for destructive clear. |
| `lj_thr_yield`, retry, sleep, owner wait | Hot body remains installed across native enter/leave. Internal `lj_thr_tls_get` calls become the same hot getter. |
| `lj_thr_current_id` | Read tid from the leased current body. Saturating owner IDs are still required. |

### `lj_thr_get_tg_fallback()` callers

`G2TG(g)` and the explicit uses in `lj_state_resumeclaim`, safepoint leader/self
selection, GC2 worker control, and `lj_thr_current_id` may return:

- the cached leased body when its immutable `gl` association equals `g`; or
- `g->main_tg` when TLS is empty or belongs to another universe.

The fallback main pointer is protected by the caller's universe lifetime, not a
new TLS borrow. It must never be returned for a stale/freed `g`. A cached body
must be cleared before it becomes DEAD/RETIRED, so the hot path need not load a
registry token or `TGF_DEAD` on every access.

### Direct `L2TG` and `tg_hint`

`L2TG(L)` bypasses TLS whenever `L->tg_hint` is non-null, including VM assembly
which loads `tg_hint` directly for dispatch. That remains valid only under this
owner-local invariant:

- the state is owned by the current OS thread;
- the hint names its current TLS-bound TG (or the universe-lifetime main TG);
- the hint is published after binding install and cleared before binding
  release.

Remote JIT owner resolution currently reads `L->tg_hint` and dereferences it.
Saved hints in `LJStateClaim` and VM-event code also remain raw. Remote readers
must resolve an exact stable key and borrow; they cannot use `L2TG` as a lifetime
proof.

### Exhaustive explicit-call checklist

This is the complete source checklist from
`rg 'lj_thr_(set|get)_tg|lj_thr_tls_get' src` at the audit boundary. Declarations
and the `G2TG` macro itself are omitted; all explicit executable callers are
included.

- `src/lib_threading.c`
  - `lj_threading_shutdown`: hot current-body observation;
  - `threading_worker_cp`: ATTACHING borrow install;
  - `threading_worker_cleanup`: exact clear/move/release;
  - `threading_attach_cp`: ATTACHING borrow install;
  - `threading_attach_cleanup`: exact clear/move/release on failure;
  - `threading_attach`: empty/current hot observation;
  - `lj_threading_detach`: hot lookup followed by exact-key clear, not a blind
    raw clear.
- `src/lj_gc2.c`
  - `gc2_worker_control_lock_l`: fallback body held through native wait;
  - `gc2_worker_thr_create_l`: fallback body held through OS-thread creation;
  - `gc2_worker_stop_locked_l`: fallback body held through join;
  - `gc2_worker_main`: ATTACHING install on entry and exact clear on exit;
  - `gc2_finalizer_current_tg`: hot current-body selection;
  - `gc2_peer_wait_owned_l`: hot owner validation;
  - `lj_gc2_worker_drain`: hot worker body;
  - `gc2_fixpoint_round`: hot logical worker body when no L is supplied.
- `src/lj_safepoint.c`
  - `lj_safepoint_apply_tg`: fallback comparison for the self SSB owner;
  - `safepoint_ack_native`: fallback self held across remote list walk;
  - `safepoint_trace_tg_active`: fallback self held across trace list walk;
  - `safepoint_leader_id`: fallback tid read;
  - `safepoint_leader_lua_state`: fallback current-L read;
  - `safepoint_leader_enter`: direct hot contender self;
  - `lj_safepoint_handshake`: both final self-SSB flush lookups.
- `src/lj_ccallback.c`
  - `lj_ccallback_prepare`: initial hot carrier and post-auto-attach hot carrier;
  - `lj_ccallback_unwind`, `lj_ccallback_enter`, `lj_ccallback_leave`: hot
    carrier body, never retained past detach.
- `src/lj_profile.c`
  - `profile_trigger`: POSIX signal-safe hot-body-only lookup.
- `src/lj_arena.c`
  - `arena_allocf_free`: hot current body compared with allocator owner.
- `src/lj_gc.c`
  - `lj_gc_flush_root_pending`: hot self exception beside registry enumeration;
  - `gc_linkobj_pending`, `lj_gc_linkobj_new_chain`,
    `lj_gc_linkobj_new_after_main`: hot owner-local pending-root publisher.
- `src/lj_str.c`
  - `strtab_active_on_hdr`, `strtab_active_on_hdr_before`,
    `strq_active_on_hdr`: hot self exception beside registry enumeration;
  - `str_body_retire_new`: hot allocator owner.
- `src/lj_tab.c`
  - `tab_struct_tid`: hot owner ID when no L is supplied;
  - `tab_mt_concurrent`: hot body-to-global observation.
- `src/lj_state.c`
  - `lj_vm_cpcall`: fallback hint for an ownerless state;
  - `close_state`: exact main-key query and clear, replacing pointer equality.
- `src/lj_tg.c`
  - `lj_tg_init`: main body borrow/install only after body publication;
  - `lj_tg_derive_prng`: hot parent snapshot;
  - `lj_tg_detach`: exact current-key clear as part of the split state machine;
  - `tg_reclaim_dead_admissible`: hot main-self observation.
- `src/lj_thr.c`
  - `lj_thr_current_id`: fallback tid;
  - `lj_thr_get_tg` and `lj_thr_get_tg_fallback`: implementation hot loads;
  - `lj_state_resumeclaim`: fallback owner hint;
  - `lj_state_owner_wait`, `lj_thr_yield`, `lj_thr_sleep_ns`: internal hot body
    held across native enter/leave.

No explicit getter caller is allowed to copy its returned body into a global or
cross-thread record. The existing sites which enumerate peers do not gain peer
lifetime from their safe self lookup; each peer still needs an exact scoped
borrow.

## Remaining raw TG holders which block positive reclaim authority

TLS conversion is insufficient until these are migrated or kept as explicit
legacy vetoes:

- `lua_State::tg_hint` and saved claim/event hints;
- `LJThread::tg`, especially userdata `__gc`, which probes raw list membership
  and can directly finalize/free the body;
- `GC2State.worker_tg[]` and the worker-retired raw chain;
- `GC2SSBNode::owner` and embedded-node publications;
- arena allocator `owner_tg`, allocation descriptors, and ownership transfer;
- `TGState::next_tg` and every legacy `gc2.tg_list` walk;
- `lj_tg_find_owner()` and JIT owner resolution;
- recorder/native/FFI records which retain TG or L beyond an owner-local call.

`LJThread::tg` should become a keyed lifecycle owner/control record, not a
cached body. `worker_tg[]` should store a stable key or a control object which
owns one scoped borrow. Each published embedded SSB node must carry or account
for a body lease until drain. Arena ownership must be transferred or leased
before the TG can enter RECLAIMING.

The 32-bit `tid`/`thr_owner` path has an ABA hazard on wrap. Saturating allocation
prevents new reuse, but eventual exact owner publications should carry a stable
TG key/incarnation beside any compact owner ID.

## Primitive/API gaps

The standalone registry primitive still needs runtime-level support for:

1. An immutable registry-spine manager which claims reusable EMPTY slots,
   allocates/links a new stable slot when necessary, and frees slots only at a
   proven universe-terminal boundary.
2. Explicit move helpers for `LJTGRegistryBorrow`; current documentation says an
   active handle must not be copied, while TLS install/clear needs a semantic
   move.
3. A release-to-completion helper which retries `LJ_TGSLOT_LOST` while the held
   lease proves the key cannot become stale. Impossible INVALID/STALE outcomes
   must retain the handle and pin/no-reclaim rather than discard it.
4. A keyed fail-closed pin helper for TLS metadata corruption or platform
   publication failure.
5. An exact current-binding key query for shutdown/detach; raw pointer equality
   is not enough.
6. Runtime association validation that `borrow.body`, `TGState.registry_key`,
   and `TGState.gl` all describe the same immutable body before TLS publication.
7. A controller handoff for the exceptional case where a joined Windows thread
   could not clear an already-installed body key.
8. Universe lifetime authority. A TG body borrow deliberately does not protect
   `global_State`/`GG_State`. Racy `lua_close()` from another OS thread can leave
   a main-TG TLS borrow elsewhere. Because the main TG is embedded in `GG_State`,
   the universe cannot be freed safely merely by pinning its TG slot. Until a
   stable universe lease/control block exists, shutdown must prove every binding
   gone or fail closed by retaining the whole universe allocation.

The last item is not optional for the project's stated racy-API safety goal.
Stable TG keys prevent body ABA, but a stale `tg->gl` pointer can equal a newly
reused universe address unless the universe itself has a non-reused identity and
lifetime lease.

## Deterministic coverage

Add a focused TLS/registry fixture with schedule hooks at borrow acquisition,
metadata install, hot-body publication, hot-body clear, and old-borrow release.
It should cover:

1. Install raises lease count from owner-only to owner+TLS; getter returns the
   exact body without changing the token.
2. RETIRED reclaim is BUSY while the TLS borrow remains and succeeds after exact
   clear/release.
3. Swap pauses before and after the hot store; observations are old-protected or
   new-protected, never unleased.
4. Clear pauses after hot `NULL` but before release; getter is null and reclaim
   remains blocked by the moved local handle.
5. Same-address slot reuse cannot make a stale binding key release the new
   incarnation.
6. Exact-key mismatch cannot clear a different universe's current binding.
7. Main A bound, main B created on the same OS thread: `G2TG(gA)` resolves A,
   `G2TG(gB)` resolves main B, closing B preserves A, and closing A clears A.
8. POSIX `raise(SIGPROF)` at each install/swap/clear hook observes only a body
   with a live lease or null. The handler performs no CX16 operation.
9. Callback auto-attach installs exactly one lease, nested callbacks reuse it,
   unwind/leave clears it, and `errno`/`LastError` are unchanged.
10. Spawn, foreign attach failure, GC-worker startup failure, and protected
    callback attach failure publish no `tg_hint` or TG roots after a TLS error.
11. Detach schedule pauses after every line in the state machine; no reclaimer
    reaches RECLAIMING before state owner/hint/TLS final use ends.
12. A deliberately abandoned TLS binding keeps reclaim BUSY/PINNED and never
    produces a UAF. Terminal shutdown detects the outstanding lease and does not
    free the registry/universe beneath it.

For Windows/Wine, inject `TlsAlloc` failure at each of the three initialization
indices and `TlsSetValue` failure at slot/inc/body publication and clear. Verify
that input/output borrow ownership is exact after every failure. Disassemble the
hot getter and require one `TlsGetValue(body_key)` call and no metadata lookup,
allocation, or CX16 helper.

For Linux and macOS/Darling, disassemble the hot getter and require one TLS-
relative body load. Run the fixture under GCC and Clang, ASAN/UBSAN, TSAN where
supported, Wine, and Darling. Add a microbenchmark comparing `G2TG` and `L2TG`
against the current raw-TLS baseline; install/clear cost is cold-path and should
be reported separately.

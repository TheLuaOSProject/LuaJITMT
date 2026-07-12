# Exact TG TLS lifecycle migration audit

Date: 2026-07-12

Status: audit/design only. This note changes no runtime code and does not make
the stable TG registry positive reclamation authority. It records the required
production migration around the dormant tagged-word TLS primitive implemented
in `src/lj_thr.c` and described in
`notes/tg-registry-tls-tagged-binding-2026-07-12.md`.

The supported target for this audit is x86-64 Linux, macOS, and desktop
Windows. Line numbers are those at the audit boundary and will move as the
production migration lands.

## Outcome

The exact TLS primitive is suitable as the lifetime-bearing hot cache, but the
production migration cannot be a mechanical replacement of
`lj_thr_set_tg()`. TG preparation, registry publication, TLS lease transfer,
root publication, state ownership, and retirement currently overlap in the
wrong order.

The smallest safe migration is one atomic caller slice which:

1. splits attach into private initialization, exact registry preparation,
   TLS install/swap, and root/LIVE commit;
2. splits detach into DETACHING/quiesce, root/state release, exact TLS
   clear/release, and DEAD/RETIRED completion;
3. rejects registry-slot OOM instead of creating a keyless raw-TLS TG;
4. gives every binding explicit universe-lifetime authority; and
5. migrates every production raw setter in the same change.

No active `LJTGRegistryBorrow` may remain in a stack local across a LuaJIT
longjmp. Borrow-to-install and clear-to-release spans must contain no throwing
operation.

## P0 correctness blockers

### Main TG is cached before it is initialized or keyed

`lj_tg_init()` publishes the raw main pointer at `src/lj_tg.c:205-206`, but
`tg_init_common()` does not initialize `tg->gl`, roots, or the embedded
registry key until `src/lj_tg.c:218`. The main key is created only when
`lj_gc2_init()` calls `lj_tg_attach()` at `src/lj_gc2.c:982`.

The raw store must leave `lj_tg_init()`. Main initialization must complete,
the registry must be initialized, and the main body/key must be published as
ATTACHING before acquiring and installing a borrow. If the OS thread already
has an exact binding for a different Lua universe, the new main TG is committed
without replacing it and remains reachable through `L->tg_hint` and the
universe-lifetime fallback.

### Spawned worker enters protected VM code before exact binding

`ThreadingWorkerCtx` is at `src/lib_threading.c:772-782`. The child currently
enters `lj_vm_cpcall()` at `src/lib_threading.c:921` and only then executes the
raw store at `src/lib_threading.c:796`. It publishes `L->tg_hint`, TG roots,
and state ownership at `src/lib_threading.c:798-810` before attach at
`src/lib_threading.c:811`.

This is too late even if the raw store is the first line of
`threading_worker_cp()`: the protected-call assembly may already resolve
`L2TG(L)`. The parent/controller must prepare the exact ATTACHING body/key, and
the child must borrow/install before entering `lj_vm_cpcall()` or any VM/JIT/
native helper.

`lj_tg_init_thread()` also writes `L->tg_hint` eagerly at
`src/lj_tg.c:234-255`. It needs a private mode which initializes the TG without
publishing a state hint or TG root. Stack rehoming must accept the intended TG
explicitly instead of depending on the premature hint.

### Foreign attach has the same prebinding problem

`ThreadingAttachCtx` is at `src/lib_threading.c:948-958`. The current path
initializes the TG with `L` at `src/lib_threading.c:2032`, so the hint is
already visible before the protected call. Inside the call it publishes the
hint and raw TLS at `src/lib_threading.c:965-967`, TG roots at `:968-971`, and
only attaches at `:980`.

Foreign attach already owns a useful universe token: `mt_entering` is retained
until the successful path transfers protection to `mt_live`. It should use
that authority to initialize privately, prepare ATTACHING, borrow, and install
outside `lj_vm_cpcall()`. Only then should it publish `L->tg_hint` and enter the
protected attach commit.

### GC workers publish raw TLS before attach

`gc2_worker_main()` stores raw TLS and enters native state at
`src/lj_gc2.c:1674-1675`, then attaches at `:1676`. The controller creates and
publishes bare `worker_thread[]`/`worker_tg[]` records at
`src/lj_gc2.c:1439-1462`; their fields are declared at
`src/lj_obj.h:1411-1413`.

The controller should prepare the exact key before thread creation. The child
must acquire/install before `lj_native_enter()`, then commit attach and publish
a per-worker success result. The aggregate `worker_started` count cannot
distinguish a keyed TLS startup failure.

### Partial setter migration is fail-stop

The tagged-word primitive deliberately aborts if the raw compatibility setter
tries to overwrite an exact binding. These production sites must migrate in
one slice:

- main bootstrap: `src/lj_tg.c:205-206`;
- spawned worker install/clear: `src/lib_threading.c:796,899`;
- foreign install/failure clear: `src/lib_threading.c:966,1021`;
- explicit foreign duplicate clear: `src/lib_threading.c:2092`;
- GC worker install/clear: `src/lj_gc2.c:1674,1718`;
- internal monolithic detach clear: `src/lj_tg.c:577-578`; and
- main close: `src/lj_state.c:509-510`.

Once any path installs an exact tag, a later `lj_thr_set_tg(NULL)` is not an
idempotent cleanup: it is an attempted loss of a live registry lease.

### Monolithic detach retires ownership too early

`lj_tg_detach()` at `src/lj_tg.c:524-587` currently performs request
acknowledgement, SSB/accounting/string/tmpbuf flush, root clearing, raw TLS
clear, DEAD publication, thread-count decrement, and RETIRED publication in
one routine. Spawned and foreign callers separately clear `L->tg_hint` and
release `lua_State` ownership around it at:

- `src/lib_threading.c:879-899`;
- `src/lib_threading.c:1007-1021`; and
- `src/lib_threading.c:2087-2093`.

The exact TLS borrow must remain installed while final owner-side state and TG
work executes. RETIRED must be the last lifecycle edge after hints, state
ownership, TLS, and final local use have ended.

### Registry-shadow OOM has no exact key

`tg_registry_link_attaching()` at `src/lj_tg.c:367-400` sets
`registry_shadow_missed` and continues on allocation failure. Such a TG cannot
be borrowed or represented by an exact tagged binding. The authoritative
migration must turn this into a normal admission failure:

- `lua_newstate()` returns `NULL` for main-slot failure;
- spawn reports its existing startup/allocation error;
- foreign attach returns false; and
- GC-worker pool creation reports failure and joins already-created workers.

Raw legacy fallback is not safe once exact TLS is the body-lifetime cache.

### TG leases do not protect the universe

The primitive validates `TGState.registry_key`, dereferences `TGState.gl`, and
walks the universe registry spine. A TG-body borrow does not retain the
`global_State`/`GG_State` allocation. In particular, racy `lua_close()` on a
different OS thread may not see or clear the main binding resident on the
original thread.

Every installed binding therefore needs an external, non-reused universe
control lease. Closing sets a closing bit, prevents new leases, and cannot free
GG or its registry spine until every binding/guard has returned its universe
lease. Existing `mt_entering`, `mt_live`, worker-controller ownership, and
joined-thread rules provide much of the secondary-path exclusion, but they do
not cover a main binding on another OS thread or a displaced cross-universe
callback binding.

### Shared-library POSIX profiling remains unresolved

The tagged-word note records the actual artifacts:

- a static Linux object uses one `%fs` load and a mask;
- Linux PIC uses `__tls_get_addr`; and
- macOS/Darwin uses a TLV thunk even with the attempted initial-exec model.

The SIGPROF schedule passes after pre-touching TLS, but neither resolver has a
formal async-signal-safe/nonblocking guarantee. This remains a P0 before
claiming that the shared-library profiler path is fully nonblocking. Possible
resolutions are a dedicated direct signal cache, a supported loader/TLV
guarantee, or an explicit build/load constraint for the profiling-capable
artifact.

### Unclean thread exit loses ownership evidence

The exact tag is the per-binding evidence for one fungible registry count. If
the OS destroys TLS without running runtime detach, the count remains but the
handle cannot be reconstructed by a controller. This is fail-closed against
UAF, but pins reclamation and can block terminal close.

Runtime-owned POSIX workers can register an idempotent cleanup handler and
disable asynchronous cancellation. Windows `ExitThread` invokes DLL thread-
detach notification, which can perform a loader-safe, allocation-free handoff
to a preallocated controller record. `TerminateThread` does not run any
user-mode or DLL cleanup and cannot be supported safely; forced termination
must remain explicitly unsupported/fail-closed.

Foreign attachments need the same preallocated exit-control record if clean
thread return without an explicit detach is meant to be supported. Otherwise
the C API contract must require `lj_threading_detach()` and terminal shutdown
must detect rather than free beneath an abandoned binding.

## Four-stage attach protocol

### Stage A: private initialization

The lifecycle owner completely initializes the TG without publishing
`L->tg_hint`, `cur_L`, `thread_L`, `thread_ud`, or a legacy TG-list link. The
intended state can be passed separately for later commit. Main bootstrap may
initialize its embedded state early because the universe is not externally
reachable, but it must not publish raw TLS.

### Stage B: exact registry preparation and borrow

While holding lifecycle and universe authority:

1. claim/link an external stable slot as ATTACHING;
2. publish the initialized body and immutable `TGState.registry_key`;
3. acquire one exact ordinary `LJTGRegistryBorrow`, retrying only
   `LJ_TGSLOT_LOST`; and
4. fail the admission on OOM, denial, corruption, pinning, or exhaustion.

No other owner may move the incoming key to DETACHING before the TLS hot LP.

### Stage C: exact TLS install or scoped swap

An empty runtime worker/foreign carrier calls `lj_thr_tg_install()`. Main
bootstrap installs only when the current OS thread is empty. A scoped
cross-universe callback may call `lj_thr_tg_swap()` while retaining the
displaced active borrow in a durable guard.

On `OK`, install/swap consumes the incoming borrow. On any other result, TLS
and handle ownership are unchanged. The caller releases the incoming handle
and aborts ATTACHING. An expected nonempty main binding for another universe is
the sole normal install-mismatch exception.

### Stage D: root publication and LIVE commit

After exact binding publication:

1. publish the attach/root descriptor;
2. publish TG root fields and `L->tg_hint`;
3. run GC/safepoint activation catch-up;
4. link the legacy TG list and increment its live count; and
5. publish ATTACHING to LIVE as the final, non-throwing edge.

Potentially throwing catch-up work must precede the list/count/LIVE tail, or it
must update an explicit caller-owned transaction stage before each LP so
longjmp cleanup never needs an unsafe raw-list membership probe.

The opportunistic attach at `src/lj_gc2.c:2682` should become an invariant
check. Main and worker TGs must already be keyed, bound where required, and
committed before a collection cycle begins.

## Four-stage detach protocol

### Stage A: close lifecycle admission and quiesce

The exact key transitions LIVE to DETACHING before the first root or hint is
cleared. While state and TLS ownership remain intact:

- abort any recorder owned by the TG;
- disown callbacks where required;
- acknowledge requests;
- flush SSB, pending roots, accounting, strings, and owner-private buffers;
- exit/discard native/JIT/FFI owner state; and
- clear remotely sampled TG root fields.

DETACHING remains borrowable for already-authorized scanners.

### Stage B: end state ownership

Clear every `lua_State.tg_hint` naming the TG and release the exact
`thr_owner`. This must happen while the TLS borrow still protects the TG and
while caller lifetime authority still protects `L` and `g`.

### Stage C: clear and release exact TLS ownership

Initialize an inactive local handle and call:

```c
result = lj_thr_tg_clear(&tg->registry_key, &old_hold);
```

On `OK`, the hot word is already zero and `old_hold` owns the exact count. Do
any final owner-side TG use through the still-protected body, then call
`lj_tgregistry_release_to_completion()`.

A Windows `TLS_FAILURE` leaves the old binding installed and the output
inactive. The caller must not release, RETIRE, or exit. Any non-LOST registry
release failure leaves the local handle active; it must be moved to durable
pin/handoff storage or fail-stop rather than vanish with the C stack.

### Stage D: publish legacy death and RETIRED

After state ownership, hot TLS, returned-handle release, and final local use
have ended:

1. publish `TGF_DEAD` and the legacy live-count decrement;
2. retire safepoint/list compatibility state; and
3. exact-transition DETACHING to RETIRED last.

RETIRED closes new registry borrows. Existing remote borrows and all legacy
raw-holder/allocator/SMR vetoes must still drain before RECLAIMING.

## Required caller-owned fields

### Spawned Lua workers

Replace the `tls_set`, `tg_state_set`, and `attached` ambiguity in
`ThreadingWorkerCtx` (`src/lib_threading.c:772-782`) with an explicit lifecycle
stage. `LJThread` currently publishes a raw `TGState *tg` at
`src/lj_thr.h:202-218`; retain it only as legacy storage ownership while adding
an exact key/control record for startup, joined cleanup, and abnormal-exit
handoff.

The child owns the attach/detach lifecycle after OS-thread start. Parent
STOPREQ/start-abort paths only signal and join; they must never retire the body
under a child that is between borrow and TLS publication.

### Foreign attachment

`ThreadingAttachCtx` needs the exact key and lifecycle stage. Its existing
`entering` and `gc_entered` fields model the universe-lifetime transfer and
should remain explicit. The success path releases `mt_entering` only after
`mt_live` and LIVE are established. Every protected failure path unwinds the
recorded exact stage, clears/releases TLS if installed, releases state
ownership, then releases the final universe token.

### GC workers

Replace each bare `worker_thread[]`/`worker_tg[]` pair with, or point it at, a
control record containing:

- `LJThr`;
- TG body and immutable key;
- lifecycle stage;
- startup result;
- exit/handoff state; and
- controller-owned storage authority.

The controller does not release/free this record until join succeeds and the
TG has passed registry and legacy reclamation.

### Main TG

No separate main borrow field is required: the tagged TLS word owns the count
and the embedded key identifies it. `close_state()` must query exact current
identity, clear only if it equals the main key, release the returned borrow,
and only then call `lj_tg_registry_main_close_begin()` and destroy subordinate
storage.

Pointer equality at `src/lj_state.c:509` is insufficient across incarnation
reuse. Closing universe B while A is current must preserve A. The shutdown
assertion at `src/lib_threading.c:591` must also permit a current TG belonging
to another universe while still rejecting a secondary TG of the universe
being closed.

### FFI callback binding guard

`CCallbackFrame` and `CCallbackRuntime` are at
`src/lj_ctype.h:389-409`. Their `auto_detach` bit cannot represent a displaced
exact borrow. Add a linear guard containing:

- target exact key;
- active displaced borrow, if any;
- install-versus-swap mode;
- whether this scope created/owns a foreign TG attachment; and
- universe-lifetime leases for both active and displaced universes.

Move this guard into the callback frame before any operation that may throw.

## FFI cross-universe reentrancy

`lj_ccallback_prepare()` reads current TLS at `src/lj_ccallback.c:880`. It only
attempts auto-attach when TLS is null at `:883`; a valid current binding for a
different universe reaches the rejection at `:887-889`. This breaks a natural
case where one native stack reenters a callback owned by another Lua universe.

The scoped protocol is:

1. acquire/prepare target B while A remains bound;
2. call `lj_thr_tg_swap(A-key, &B-hold, &saved_A_hold)`;
3. move `saved_A_hold` into the durable callback guard;
4. execute B, with nested B callbacks reusing the binding;
5. on normal leave or `lj_ccallback_unwind()`, swap B back to A; and
6. release the returned B hold and finish any B attachment lifecycle.

Nested A to B to C callbacks form a guard stack. Both normal leave at
`src/lj_ccallback.c:1258-1311` and unwind at `src/lj_ccallback.c:901-940` must
restore exactly once. The existing save/restore of `errno` and Win32
`LastError` must continue to surround all TLS lookup/swap/detach work.

Normal spawned, foreign, GC-worker, and main lifecycle paths use install/clear,
not swap. Swap is for scoped reentrancy or a future general API-entry binding
stack.

## Complete production caller inventory

### Raw setters and lifecycle sites

| Caller | Current site | Exact migration |
| --- | --- | --- |
| Main bootstrap | `src/lj_tg.c:205-206` | Remove; prepare/borrow/install after registry initialization. |
| Spawn install | `src/lib_threading.c:796` | Install before `lj_vm_cpcall()`. |
| Spawn clear | `src/lib_threading.c:899` | Split detach, exact clear/release. |
| Foreign install | `src/lib_threading.c:966` | Prepare/install before protected VM entry. |
| Foreign failure clear | `src/lib_threading.c:1021` | Exact staged cleanup. |
| Explicit foreign clear | `src/lib_threading.c:2092` | Remove duplicate; split detach owns exact clear. |
| GC-worker install | `src/lj_gc2.c:1674` | Exact install before native entry. |
| GC-worker clear | `src/lj_gc2.c:1718` | Exact clear/release before exited publication. |
| Monolithic detach clear | `src/lj_tg.c:577-578` | Remove from generic detach body. |
| Main close | `src/lj_state.c:509-510` | Exact key query, clear, release. |

Production attach/commit callers are:

- main at `src/lj_gc2.c:982`;
- spawned worker at `src/lib_threading.c:811`;
- foreign attach at `src/lib_threading.c:980`;
- GC worker at `src/lj_gc2.c:1676`; and
- the late mark-start fallback at `src/lj_gc2.c:2682`, which should become an
  invariant check.

Production detach callers are:

- spawned failure/exit at `src/lib_threading.c:879,897`;
- foreign protected cleanup at `src/lib_threading.c:1007,1017`;
- explicit foreign detach at `src/lib_threading.c:2087,2091`;
- GC worker at `src/lj_gc2.c:1717`; and
- main terminal registry close at `src/lj_tg.c:896-913`.

### Direct hot getters

These remain non-borrowing hot observations. Their safety comes from the one
ordinary lease represented by the exact TLS tag for the entire call:

- FFI callback carrier selection and validation:
  `src/lj_ccallback.c:880,884,908,1204,1285`;
- safepoint contender self lookup: `src/lj_safepoint.c:705`;
- POSIX SIGPROF: `src/lj_profile.c:402`;
- arena owner comparison: `src/lj_arena.c:3323`;
- table owner/concurrency helpers: `src/lj_tab.c:107,148`;
- string active-header/allocator helpers:
  `src/lj_str.c:379,416,467,1178`;
- PRNG parent and reclaim self checks: `src/lj_tg.c:259,667`;
- pending-root helpers: `src/lj_gc.c:2138,2184,2225,2250`;
- GC2 finalizer/owner/worker/fixpoint helpers:
  `src/lj_gc2.c:2973,3354,12235,12314`; and
- shutdown/attach/detach observations:
  `src/lib_threading.c:583,1999,2077`.

The destructive identity callers at `src/lj_state.c:509`,
`src/lj_tg.c:577`, and `src/lib_threading.c:2077` must query the exact key
before changing TLS or lifecycle state.

Equivalent internal hot reads occur in owner wait/yield/sleep at
`src/lj_thr.c:649,699,727`.

### Fallback callers

Explicit `lj_thr_get_tg_fallback()` callers are:

- `src/lj_state.c:109`;
- `src/lj_safepoint.c:230,610,628,669,677,779,799`;
- `src/lj_gc2.c:1329,1385,1610`; and
- `src/lj_thr.c:224,567`.

`G2TG()` at `src/lj_obj.h:2108` expands to the same helper throughout the VM.
The cached exact body is protected by TLS; the `g->main_tg` fallback is
protected only by the caller's universe lifetime and must never be used with a
stale `g`.

The exact TLS migration does not make peer bodies visited through
`gc2.tg_list` safe. String, GC-root, safepoint, arena, worker, SSB, and other
remote holders still need scoped registry borrows or must remain explicit
legacy reclamation vetoes.

## Error, longjmp, and exit matrix

### Exact borrow/install

- Retry only `LJ_TGSLOT_LOST` while lifecycle authority proves the key cannot
  retire.
- OOM, BUSY after completed body publication, DENIED, STALE, PINNED,
  EXHAUSTED, or corruption aborts the attachment transaction.
- `LJ_THR_TG_OK` consumes the input borrow.
- `EXPECT_MISMATCH`, `INVALID`, `CORRUPT`, and `TLS_FAILURE` leave TLS and all
  handles unchanged; release the still-active input before aborting ATTACHING.
- Only main bootstrap may treat an exact binding for another universe as a
  normal install mismatch.

### Exact clear/release

- Worker/foreign/GC clear mismatch is an invariant failure, not evidence that
  cleanup already happened.
- Main close may skip clear only after exact proof that the current binding is
  empty or belongs to another universe.
- Windows `TLS_FAILURE` leaves the old binding installed and output inactive;
  do not retire or exit.
- `lj_tgregistry_release_to_completion()` consumes on OK and retries LOST. Any
  other result leaves the handle active and requires durable pin/handoff or
  fail-stop.

### Protected failures

- Spawned worker `lj_vm_cpcall()` returns every Lua error to
  `threading_worker_cleanup()`, whose exact stage determines cleanup.
- Parent STOPREQ/start-abort paths join; they never free or retire a child
  control record before the child cleanup runs.
- Foreign attach retains `mt_entering` through every protected failure cleanup
  and transfers it to `mt_live` only on success.
- Attach commit should have a non-throwing publication tail so cleanup never
  needs `threading_tg_is_registered()` raw-list discovery.
- FFI setup/body/result-conversion errors restore their binding guard from
  `lj_ccallback_unwind()`.

## Windows backend analysis

### Dynamic tagged value directly in a TlsAlloc slot

The implemented primitive uses one process-lifetime `TlsAlloc` index. One
`TlsSetValue` is the install/swap/clear LP. This removes the old multi-index
rollback problem, but it cannot make detach infallible: the documented API can
return failure. A failed clear leaves the old hot exact binding and its lease
installed, so releasing or RETIRING would permit UAF.

Microsoft documents the success/failure result here:

- <https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlssetvalue>

Sound responses are limited to same-thread retry/park, durable pin/leak plus
universe retention, or fail-stop.

### Native PE compiler TLS

A scalar native PE TLS word makes hot get/store direct and makes swap/clear
mutation infallible once the module's TLS block exists. Modern Windows loader
support makes this justified for a deliberately modern desktop x86-64 target.
Microsoft's PE documentation notes the Vista-era improvements for dynamically
loaded DLLs:

- <https://learn.microsoft.com/en-us/windows/win32/debug/pe-format>

A local experiment built a Clang native-TLS DLL, created an OS thread before
`LoadLibrary`, then verified independent zero initialization and get/set on
both the pre-existing and loading threads under Wine 10. The getter/setter
disassembled to direct GS-based accesses.

Compatibility constraints:

- pre-Vista/XP x64 explicit loading must be excluded;
- Microsoft's delay-load linker documentation still warns that static TLS is
  not handled by the delay-load helper, so `/DELAYLOAD` needs an explicit test
  or exclusion:
  <https://learn.microsoft.com/en-us/cpp/build/reference/linker-support-for-delay-loaded-dlls>;
- `/clr`, WinRT/UWP variants, and `FreeLibrary` with live bindings are outside
  the native desktop contract; and
- MinGW-w64 GCC 14 lowers `__thread` through
  `__emutls_get_address` and ignores `__declspec(thread)`, so a simple macro
  switch is neither direct nor allocation-free. Native TLS requires Clang/
  MSVC or a deliberate PE-TLS implementation for GCC.

### Direct TEB slot access

Retaining `TlsAlloc` but directly reading/writing `TEB.TlsSlots[64]` or
`TlsExpansionSlots` would make post-admission mutation infallible on current
desktop implementations. For key 64 under Wine 10, a successful
`TlsSetValue(key, NULL)` on a fresh thread allocated `TlsExpansionSlots`, and a
subsequent non-null set matched the direct array entry.

This must not be the default correctness authority. Microsoft explicitly says
the TEB structure may change, tells applications not to access these members
directly, and directs them to `TlsGetValue`:

- <https://learn.microsoft.com/en-us/windows/win32/api/winternl/ns-winternl-teb>

Microsoft also says a `TlsAlloc` result is opaque and must not be assumed to be
a zero-based array index:

- <https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-tlsalloc>

Other concerns are UWP versions which map TLS calls to FLS behavior, private
SDK/MinGW TEB layout variation, bypassed sanitizer/API interception, and an
undocumented assumption that every future successful set uses the observed
array layout. Fibers are otherwise compatible with desktop TLS because the
desired binding is OS-thread scoped, not fiber scoped.

Direct TEB access is reasonable only as an opt-in backend with runtime
self-tests against `TlsGetValue`, version/build gating, and a fully supported
fallback. A self-test still cannot turn the private layout into a contractual
ABI.

### Process-lifetime per-thread cell

The safest cross-toolchain dynamic backend is:

1. reserve one process-lifetime `TlsAlloc` index;
2. on first owner admission for an OS thread, allocate a process-lifetime
   `LJThrTGCell` and publish its pointer with `TlsSetValue`;
3. propagate allocation/set failure before TG/root publication; and
4. thereafter mutate only `cell->tagged_word` for install/swap/clear.

The hot getter costs one `TlsGetValue` and one cell load, but clear/swap cannot
fail. The cell can also hold a preallocated abnormal-thread-exit handoff record
and is reusable across sequential Lua universes. Runtime-owned cells can be
freed by the joined controller; foreign cells need clean DLL-thread-detach
handoff or may remain process-lifetime allocations.

### Windows thread termination

`ExitThread` invokes each attached DLL's thread-detach entry point, permitting
an allocation-free tagged-word-to-controller handoff:

- <https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-exitthread>

`TerminateThread` runs no user-mode cleanup and does not notify DLLs; it cannot
be made a safe Lua/TG lifecycle operation:

- <https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-terminatethread>

The detach notification must only clear/move preallocated ownership and push a
lock-free handoff. It must not allocate, wait, run Lua cleanup, or perform a
full TG detach under the loader lock. The joined controller completes root/
state/lifecycle cleanup afterward.

## P1 follow-on lifetime work

Exact TLS protects only the current OS thread's hot body. Positive TG
reclamation still requires migration or explicit legacy vetoes for:

- `lua_State::tg_hint` at `src/lj_obj.h:2097` and saved claim/event hints;
- `LJThread::tg` at `src/lj_thr.h:206`;
- `GC2State.worker_tg[]` and `worker_tg_retired` at
  `src/lj_obj.h:1412-1413`;
- `GC2SSBNode::owner`;
- arena `owner_tg` and allocation/transfer records;
- `TGState::next_tg` and every legacy list traversal;
- JIT/recorder/native/FFI records which retain TG or L beyond an owner-local
  call; and
- compact `tid` ownership publications where exact incarnation identity is
  required.

These do not prevent landing the exact TLS caller migration while the existing
legacy gates remain mandatory positive authority. They do prevent claiming
that the stable registry alone authorizes physical TG reclamation.

## Required deterministic tests

The primitive fixture already covers tagged install/swap/clear ownership, two
keyed OS threads, exact mismatch, same-address reincarnation, POSIX SIGPROF
schedule, and Windows one-word failure injection. Its corrupt-token case also
passes ASAN/UBSAN and GCC TSAN after fixing an uninitialized snapshot found
during this audit.

Production integration still needs:

1. Main A and B on one OS thread: `G2TG(A)` resolves A, `G2TG(B)` resolves B,
   closing B preserves A, closing A clears A, and callbacks for B work while A
   was displaced.
2. Spawn success, exact-install failure, state-claim failure, protected error,
   STOPREQ before start release, ordinary Lua error, and normal return. After
   join, the TLS count is gone and no hint/root names the TG.
3. Foreign attach failure before and after prepare, borrow, install, GC/live
   admission, and commit, including concurrent `lua_close()`.
4. A detach fixture paused after every stage while another thread attempts
   registry borrow/reclaim. RECLAIMING must remain impossible until state,
   TLS, and final local use end.
5. GC worker per-slot install/startup failure, partial-pool cleanup, normal
   stop, failed join retry, and exact clear before `worker_exited` publication.
6. Nested A to B to C FFI callbacks with normal leave and setup/body/result
   conversion unwind. Each guard restores once and preserves `errno` and
   `LastError`.
7. POSIX `raise(SIGPROF)` at attach/install, swap, hot clear, returned-handle
   release, and detach/RETIRE boundaries. Test static Linux, Linux PIC, and
   Darling artifacts separately.
8. Windows backend tests for first-admission allocation/set failure, repeated
   binding reuse, a thread predating `LoadLibrary`, Wine, and native Windows.
9. Clean `pthread_exit`/`ExitThread` handoff. Forced `TerminateThread` is
   documented unsupported and must never lead to reclamation.
10. Registry-slot OOM at main, spawn, foreign, and GC-worker preparation.
    Verify no raw TLS, hint, TG roots, or LIVE list/count publication remains.
11. Exact-key mismatch at main close and every secondary detach; a different
    universe's binding must never be cleared.
12. Lease/counter assertions after every error and join, plus terminal close
    detection of an intentionally abandoned binding.
13. Disassembly/performance gates: Linux static direct TLS, Linux PIC chosen
    resolution, Darwin chosen resolution, Windows native TLS or cell backend,
    and cold install/clear cost reported separately from hot getter cost.
14. A CI source check requiring zero production `lj_thr_set_tg()` callers.
    Only the primitive raw/exact exclusion fixture may retain the compatibility
    setter.

Many existing C fixtures directly replace raw TLS pointers. They must migrate
to exact prepare/borrow/install/swap/clear helpers or explicitly remain isolated
raw-compatibility tests; otherwise they bypass the lifecycle being verified.

## Migration acceptance boundary

The production slice is ready only when all of the following are true:

- every production setter listed above is removed;
- every incoming TG has a key before TLS/root publication;
- attach failures roll back by exact stage without raw list guessing;
- every detach clears state, exact TLS, and its returned lease before RETIRED;
- main close uses exact key identity and a universe lifetime lease;
- Windows detach has an infallible post-admission store backend or an explicit
  fail-stop/pin policy which cannot be mistaken for successful detach;
- POSIX shared-library SIGPROF has a justified nonblocking hot lookup; and
- abnormal clean thread exit either hands ownership to a controller or remains
  detected and unreclaimable.

Until then, the tagged TLS primitive remains correctly dormant and the raw
compatibility setter must not be mixed with exact production bindings.

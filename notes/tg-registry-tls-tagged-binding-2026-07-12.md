# Tagged stable-TG TLS binding primitive

Date: 2026-07-12

Status: implemented and tested as a dormant migration primitive. Production
main/spawn/foreign/GC-worker lifecycle callers still use the raw compatibility
setter. The stable registry remains an additional negative reclamation veto;
legacy list, SMR, allocator, worker, and raw-holder gates remain positive
authority.

## Result and design divergence

The implemented binding uses one tagged word, not the three Windows TLS
indices proposed in `tg-registry-tls-binding-design-2026-07-11.md`. POSIX
keeps the word directly in compiler TLS. Windows keeps it in a stable
per-thread cell and stores the cell pointer in one process TLS index:

```text
0                 empty
TGState *          raw compatibility binding (bit 0 = 0)
TGState * | 1      exact binding which owns one ordinary registry lease
```

This supersedes only that note's physical TLS representation and Windows
rollback machinery. It preserves the exact-key, root-publication, attach,
detach, and caller-lifetime requirements.

The divergence removes two metadata indices, split publication, rollback,
dirty metadata, and ambiguous post-failure ownership. Install, swap, and clear
each have one tagged-word linearization write. The hot getter performs one TLS
lookup/load and masks bit zero. Windows first admission allocates and publishes
the stable cell; after that, binding reads and writes never allocate or call
`TlsSetValue`. Exact operations never borrow or touch a CX16 token at the TLS
edge. A POSIX platform TLS resolver may still perform lazy setup, as documented
for the unresolved PIC/TLV paths below.

## Target and pointer-tag contract

This phase supports x86-64 Linux, macOS, and Windows. Compile-time assertions
require:

- `alignof(TGState) >= 2`, reserving bit zero for the exact-binding tag;
- `sizeof(uintptr_t) == sizeof(void *)`;
- every runtime TG body follows its declared C alignment; and
- a tagged integer is never dereferenced before bit zero is removed.

Win32 `TlsSetValue` treats the value as opaque. Converting the x86-64 pointer
to `uintptr_t`, setting/clearing bit zero, and converting the masked value back
is an explicit target ABI contract. The raw compatibility setter rejects an
odd TG pointer.

`CORRUPT` handling assumes an exact word was minted by this API and its
registry/token associations later drifted. The reserved tag-only word and an
under-aligned decoded body are rejected before dereference. An arbitrary forged
odd but otherwise aligned machine word may still fault while recovering the
embedded key; a one-word encoding cannot validate an arbitrary address before
reading it. This is the internal-memory-corruption boundary, not an ordinary
Lua race or ABA case.

## Fungible-count ownership proof

Registry lease counts are fungible; the token does not identify which borrower
owns which increment. The tagged word is the per-binding ownership evidence:

1. `lj_tgregistry_try_borrow()` admits one exact ordinary lease and returns a
   linear `{key, body, active}` handle.
2. A successful install consumes exactly that handle and publishes `body|1`.
   No token operation occurs at the TLS edge. The tag now represents exactly
   the consumed count.
3. While that count exists, RETIRED cannot reach owner-only count one, so the
   body cannot enter RECLAIMING, be cleared, or be reused. Consequently the
   body's embedded `registry_key` remains immutable and names the same slot
   incarnation.
4. Clear first publishes zero. The same OS thread then reconstructs one active
   linear handle from the still-protected body and embedded key. The count did
   not disappear during this short representation move; a later
   `lj_tgregistry_release_to_completion()` consumes it exactly once.
5. Swap starts with the old tagged count and a distinct active new handle. Both
   counts exist when it publishes `new_body|1`. It then reconstructs the old
   output handle and invalidates the new input. Thus either hot body observed
   at the edge has a corresponding live count.

All exact APIs are same-thread serialized control-plane operations. Copying a
tagged word to another TLS cell without acquiring another lease would violate
the contract and is not exposed by the API.

Same-address slot/body reuse is safe: reuse cannot begin while the tagged
count exists, and after reuse an old expected incarnation cannot clear the new
tag because `TGState.registry_key` and the expected key differ.

## API results and linearization

The result contract in `lj_thr.h` is unambiguous:

- `OK` install/swap consumes `new_hold`;
- `OK` swap/clear activates `old_hold`;
- `EXPECT_MISMATCH`, `INVALID`, `CORRUPT`, and `TLS_FAILURE` change neither the
  one-word binding nor any input/output handle.

POSIX publication is one release store to compiler TLS. Windows first-cell
publication is one fallible `TlsSetValue`, before any tagged binding exists;
tagged install, swap, and clear are release stores to the admitted cell.
`TLS_FAILURE` is therefore an install-only first-admission result. Swap and
clear cannot encounter TLS publication failure after their exact view proves
the cell exists. `current_key()` is a slow exact control query, while
`lj_thr_get_tg()` only loads and masks the hot word.

Install and the new side of swap accept only ATTACHING/LIVE. Existing tagged
bindings can be queried or cleared through DETACHING/RETIRED (and conservatively
PINNED when body/key association is still exact). A token state snapshot cannot
prevent the lifecycle owner from retiring immediately afterward, so install
and swap require caller-held lifecycle publication authority through the hot
LP. All reverse validation also requires a caller-held universe lifetime while
it dereferences `TGState.gl` and walks the immutable registry spine.

Registry membership validation uses Floyd cycle detection and the null tail.
It does not use the lagging `tg_registry_nodes` telemetry count or impose an
undocumented maximum number of TGs.

## Platform implementation and hot artifacts

### Linux

The static object getter disassembles to one `%fs` TLS load and one `and`.
The PIC/shared object currently uses `__tls_get_addr`, then one load and mask.
The focused test pre-touches the binding and successfully raises `SIGPROF` after
install, swap, and clear, observing protected A, protected B, and NULL.

`__tls_get_addr` is not a formally guaranteed async-signal-safe/nonblocking API.
ELF `initial-exec` would produce a direct load but can break late `dlopen` when
static TLS surplus is unavailable. Resolving that loader-compatibility tradeoff
or providing a dedicated direct signal cache remains required before claiming
the shared-library profiling path fully nonblocking.

### macOS

The osxcross Mach-O getter uses the platform TLV thunk (`X86_64_RELOC_TLV`) and
then masks the result; Clang's `tls_model("initial-exec")` still produced a TLV
call in an artifact experiment. The same SIGPROF schedule test passed under
Darling after the binding was pre-touched. Formal nonblocking TLV behavior is
not claimed and needs a Darwin-specific direct-cache solution or a documented
platform guarantee.

### Windows

One process-lifetime `TlsAlloc` index stores `LJThrTGCell *`. The stable cell
contains one atomic `tagged_word`. `lj_thr_tg_tls_init()` initializes both the
process index and the current thread's cell. `lua_newstate()` calls it before
PRNG, owner-ID, allocator, or universe publication and returns NULL on index
allocation, cell `malloc`, or first `TlsSetValue` failure; failed `InitOnce`
remains retryable and an unpublished cell is freed.

The getter has no initialization or allocation path. The MinGW UCRT PE object
disassembles to a key load, exactly one `TlsGetValue`, a null test, one dependent
cell-word load, and one mask. There is no `InitOnce` call. The object contains
one `TlsSetValue` relocation, solely in `lj_thr_tg_tls_init()`. Existing-cell
install, swap, and clear each locate the cell with `TlsGetValue` and issue a
plain x86-64 atomic release store; none calls `TlsSetValue`.

This is the relevant performance trade: compared with storing the tag directly
in the Win32 TLS slot, reads gain one dependent pointer load, while every
post-admission mutation loses a fallible Win32 setter call and rollback result.
No Wine timing is reported as a native-Windows performance number. The bounded
artifact result is one API lookup plus one ordinary load on the dominant read
path. At each mutation LP the setter is one API lookup plus one ordinary store;
the surrounding slow exact operation also performs its validation reads (and
install checks current-thread admission). All allocation remains confined to
first admission.

`LJ_THR_TLS_TEST_HELPERS` independently injects index allocation, cell
allocation, and first-cell publication failure. Fresh-thread tests prove that
allocation/publication failure returns `TLS_FAILURE`, leaves the incoming exact
handle active, and leaves the getter empty. A publication failure held armed
across an already-admitted thread's install, swap, and clear is consumed only
by a later fresh thread, proving those mutations do not call `TlsSetValue`.

Published cells intentionally have process lifetime in this phase. Win32 TLS
indices have no destructor, so an admitted OS thread which exits loses the only
slot reference and leaks one `LJThrTGCell` allocation until process teardown.
If it exits with an exact tag installed, the represented registry lease is also
conservatively leaked. Production lifecycle migration needs an explicit joined-
thread/controller handoff before it can reclaim cells or exact leases safely;
guessing from a TLS destructor would not preserve fungible-count ownership.
Cell allocation deliberately uses the CRT `malloc`, not custom `lua_Alloc`,
under the project's temporary allocator exception. A later runtime-owned cell
allocator must preserve failure-before-publication and process/thread-exit
ownership semantics rather than silently moving this allocation into Lua heap
lifetime.

First admission is not claimed to be nonblocking in this checkpoint:
`InitOnceExecuteOnce`, `TlsAlloc`, CRT `malloc`, and the first `TlsSetValue` may
enter operating-system or allocator synchronization. This is a cold
OS-thread-admission bridge, not permission to use those operations in the VM,
GC, JIT, FFI-call, or already-admitted TG-switch paths. Before the final
lockless claim, runtime-created threads should receive controller-prepared cell
storage and foreign admission needs an explicitly bounded allocation policy (or
an equivalent direct PE TLS implementation) whose failure occurs before any
Lua/TG publication. The post-admission binding operations implemented here do
not inherit that allocation path.

The raw void setter remains temporarily fail-stop if a spawned/foreign thread's
cell admission fails, because it cannot report failure without silently losing
the required binding. `lua_newstate()` still propagates the same failures as
NULL before publication. Production migration to result-bearing exact APIs
removes this raw-call limitation.

Native PE compiler TLS is a likely follow-up, but toolchain behavior differs.
Clang x86-64 can emit native direct GS TLS and modern Windows supports static
TLS in dynamically loaded DLLs. MinGW-w64 GCC 14 lowers `__thread` through
`__emutls_get_address` and ignores `__declspec(thread)`, so switching today
would regress the supported GCC build unless it used custom PE TLS access or
made Clang/MSVC a requirement.

## Deterministic coverage

`tests/t-tg-tls-binding.c`, registered as `m4_tg_tls_binding`, covers:

- raw-to-keyed mode exclusion and exact current-key query; the reverse
  keyed-to-raw overwrite is a checked fail-stop guard, not invoked in-process;
- reserved tag-only corruption classification without consuming a handle;
- reverse body/key/global-spine mismatch rejection without consuming input;
- rejection of an even but under-aligned incoming registry body before any
  `TGState` dereference;
- install/getter and owner+TLS lease counts;
- exact-key mismatch leaving the current binding untouched;
- swap with both old/new counts live at the tagged-word LP;
- hot NULL before returned-handle release and RETIRED reclaim exclusion;
- valid-body PINNED current-key, clear, release, and permanent no-reclaim
  behavior;
- same slot and same body address reused under a new incarnation;
- two simultaneous OS threads with distinct exact bindings and independent
  clear/release, with lease counts proving isolation;
- POSIX same-thread SIGPROF observations after install/swap/clear;
- Windows index/cell-allocation/first-publication failure ownership, including
  fresh-thread failure and existing-cell mutation isolation; and
- Linux, MinGW UCRT/Wine, and macOS/Darling builds and runtime execution.
- ASAN+UBSAN and GCC TSAN target builds, including malformed-token and
  two-thread exact-binding coverage.
- GCC and Clang Linux builds (the standalone Clang fixture uses `-mcx16` for
  the registry token's required inline 16-byte CAS contract).

Verified artifacts:

- Linux static: one `%fs` load plus mask;
- Linux PIC: `__tls_get_addr`, load, mask (open issue above);
- Windows: one `TlsGetValue`, one dependent cell load, and one mask, with no
  lazy initialization; the sole `TlsSetValue` relocation is first admission;
- macOS: one TLV resolver call, load, mask (open issue above).

## Remaining migration blockers

This primitive does not make production TG reclamation token-authoritative.
Before lifecycle callers adopt it, the runtime still needs:

- attach/root descriptors and lifecycle-owner exclusion carried through the
  exact TLS LP;
- split detach ordering which clears state hints/owners before exact TLS clear
  and RETIRED;
- keyed migration of `lua_State::tg_hint`, `LJThread::tg`, GC worker records,
  SSB owners, arenas, and every legacy remote TG-list holder;
- a stable universe lifetime lease; and
- resolution of the ELF PIC and Mach-O signal-resolver issue above.

Windows first-thread admission also retains the cold synchronization/allocation
bridge described above. It is not part of the final nonblocking proof.

There is intentionally no TLS/FLS destructor. The Windows cell allocation has
process lifetime, as detailed above. If any OS thread exits while an exact tag
is installed, the token count remains as a conservative leak but its per-thread
tag is gone. Production adoption therefore also needs a joined-thread
controller handoff or terminal outstanding-lease detection which retains the
whole universe. It must never guess-release that fungible count or free
`global_State` beneath it.

The compatibility raw setter is intentionally incompatible with an exact tag:
it aborts before overwriting or hiding a tagged lease. Production callers are
not migrated in this slice.

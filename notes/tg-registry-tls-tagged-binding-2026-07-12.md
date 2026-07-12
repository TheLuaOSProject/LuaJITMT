# Tagged stable-TG TLS binding primitive

Date: 2026-07-12

Status: implemented and tested as a dormant migration primitive. Production
main/spawn/foreign/GC-worker lifecycle callers still use the raw compatibility
setter. The stable registry remains an additional negative reclamation veto;
legacy list, SMR, allocator, worker, and raw-holder gates remain positive
authority.

## Result and design divergence

The implemented binding uses one tagged TLS word, not the three Windows TLS
indices proposed in `tg-registry-tls-binding-design-2026-07-11.md`:

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
each have one TLS-word linearization write. The hot getter performs one TLS
lookup/load and masks bit zero. The binding code itself never borrows, scans,
allocates, initializes TLS, or touches a CX16 token; a platform TLS resolver
may still perform lazy setup, as documented for the unresolved PIC/TLV paths
below.

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

POSIX publication is one release store to compiler TLS. Windows publication is
one `TlsSetValue`; failure leaves its prior value unchanged. `current_key()` is
a slow exact control query, while `lj_thr_get_tg()` only masks the hot word.

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

One process-lifetime `TlsAlloc` index stores the complete tagged word.
`lua_newstate()` explicitly initializes it before PRNG, owner-ID, allocator, or
universe publication and returns NULL on failure; failed `InitOnce` remains
retryable. The getter disassembles to a key load, exactly one `TlsGetValue`, and
one mask, with no `InitOnce` call.

`LJ_THR_TLS_TEST_HELPERS` injects the single allocation failure and the single
install/swap/clear `TlsSetValue` failure. Because publication is one word,
there is no partial metadata or rollback state. The raw void setter remains
temporarily fail-stop on a per-thread `TlsSetValue` failure because it cannot
report failure safely; production migration to the result-bearing APIs removes
that limitation.

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
- Windows allocation and one-word set failure ownership; and
- Linux, MinGW UCRT/Wine, and macOS/Darling builds and runtime execution.
- ASAN+UBSAN and GCC TSAN target builds, including malformed-token and
  two-thread exact-binding coverage.
- GCC and Clang Linux builds (the standalone Clang fixture uses `-mcx16` for
  the registry token's required inline 16-byte CAS contract).

Verified artifacts:

- Linux static: one `%fs` load plus mask;
- Linux PIC: `__tls_get_addr`, load, mask (open issue above);
- Windows: one `TlsGetValue` plus mask, no lazy initialization;
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

There is intentionally no TLS/FLS destructor. If an OS thread exits while an
exact tag is installed, the token count remains as a conservative leak but its
per-thread tag is gone. Production adoption therefore also needs a joined-
thread controller handoff or terminal outstanding-lease detection which
retains the whole universe. It must never guess-release that fungible count or
free `global_State` beneath it.

The compatibility raw setter is intentionally incompatible with an exact tag:
it aborts before overwriting or hiding a tagged lease. Production callers are
not migrated in this slice.

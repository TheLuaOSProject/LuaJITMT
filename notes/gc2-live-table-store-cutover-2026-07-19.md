# GC2 live scalar table-store cutover

Date: 2026-07-19

This b1.2.1 tranche moves scalar table mutation onto one production
publication transaction shared by the interpreter, C API, x64 VM helpers, JIT
helpers, and table-valued FFI metamethod stores. It does not edit `plan/`,
enable the retired collector, or make typed activation `COMMIT` positive
reclamation authority. The legacy GC2 close/SMR predicates remain
authoritative while the root-gate migration is incomplete.

The change deliberately removes the large FFI-specific "match C call against
an explicit shape" mechanism. FFI `__newindex` now resolves the ordinary Lua
metamethod pair and uses the same keyed table-store transaction as every other
publisher. This keeps the safety property attached to the mutation primitive,
not to a growing list of callers.

## Publication transaction

The central keyed CAS captures the key and value by value before opening any
raw-slot interval. It has three semantic modes:

- `ANY`, for an ordinary replacement or insertion after Lua semantics have
  selected a slot;
- `EXISTING`, for the JIT fast path, which must report an absent/nil value so
  the interpreter can run `__newindex`; and
- `NIL`, for expected-nil publication, which returns the observed competing
  value instead of overwriting it.

A store needs the full guard when the new value is a GC object, or when a
non-nil store publishes a GC key. Primitive array/numeric stores keep their
short CAS path. For a guarded edge the owner performs this order:

1. semantically prepublish stable copies of the key, value, and parent before
   any retry can make a trace/VM scratch location stale;
2. enter a short GC2 SMR interval and prove the candidate slot belongs to the
   currently published array/node generation with a single-attempt validator;
3. capture the exact expected old slot value and enforce `ANY`, `EXISTING`, or
   `NIL` semantics;
4. borrow the exact LIVE TG registry body, lease the parent and every GC
   operand, semantically publish the guard copies, enter weak-write protection,
   and publish the per-TG descriptor;
5. admit the descriptor against OPEN/PENDING, or exact-CAS CLOSING/COMMIT back
   to PENDING;
6. recheck the slot without waiting, then perform final gate and exact-ticket
   revalidation immediately next to the CAS;
7. CAS from the captured expected value to `guard.value`, never rereading the
   caller's scratch source;
8. on every successful CAS, including a post-CAS currentness miss, bump the
   table dirty epoch exactly once, perform weak/key/value and SWEEP rescue
   barriers, and force the durable legacy table-rescan handoff; and
9. finish the exact descriptor and release weak protection, object leases, and
   the registry borrow before leaving SMR or performing an L-aware wait.

`begin == OK` or `begin == PINNED` is not store authority. Only final
revalidation sets `store_authorized`. A retained PINNED guard can do so only
after global activation is exact absorbing NO_RECLAIM and its applicable TG,
table, key, value, and weak authorities remain owned. INVALID and RETRY never
authorize a CAS, and every no-store path owner-finishes the guard.

The central helper rejects FINREG publish-claim sentinels rather than CASing
over them. A failed CAS observes FORWARD, ABSENT, EXISTS, a publish-claim, or an
ordinary value change and returns only after clearing every descriptor
authority. The caller may then re-resolve or yield.

## Resize rule: published roots are the current authority

The descriptor-active validator is deliberately bounded: it takes acquired
array/node snapshots under SMR and never allocates, assists, waits, calls back,
or yields. Mixed observations return STALE.

Earlier revisions treated an exact successor pointer plus FORWARD/RETIRING as
one-key completion authority. That is unsafe. A hash FORWARD is visible before
the successor value is installed. For separated arrays, the resize owner can
capture old value `V`, an assistant can install `V` and publish FORWARD, a
mutator can delete the successor, and the paused owner can then resume and
resurrect `V`. Structural next-generation identity does not close that window.

The safe beta policy is therefore:

- every scalar store to an unpublished successor generation returns STALE;
- the caller may help the finite resize owner outside descriptor/SMR authority,
  but must retry from the table roots; and
- a successor slot becomes store-authoritative only after its array/node root
  is the table's acquired, non-retiring published root.

This policy is lock-free at the publication transaction but is not yet the
desired fully non-blocking resize completion. The long-term cutover needs a
persistent, uniquely owned MOVING descriptor (or an equivalent composite
resize transaction) recording the key, source value, destination, and
completion state. Only that object can prevent a captured resize-owner replay
and safely authorize pre-root mutation. Re-enabling successor writes without
that certificate is prohibited.

## Caller cutover and stable operands

JIT existing-slot stores no longer have a separate raw CAS/weak-window
implementation. They use the common `EXISTING` transaction. Expected-nil
publishers use the common `NIL` transaction. The x64 direct templates remain
restricted to primitive/number cases; GC-valued array/hash stores and
helper-backed NEWREF paths converge on the keyed core.

Several callers previously kept a raw destination pointer across a helper that
may resize, allocate, or yield, then used it for a post-store barrier. Those
barriers now use stable source TValue snapshots instead:

- x64 VM table helpers publish the stable VM source operand;
- TSETM restores stack-relative source offsets across retries and barriers the
  source range, not re-resolved destinations;
- `lua_rawseti`, library registration helpers, and FFI registry/miscmap stores
  retain stack-relative or by-value sources; and
- keyed readback enters a short SMR interval, validates before dereference,
  loads by value, revalidates, and leaves before returning.

## Owner proof, nesting, and terminal close

The scalar guard currently validates the existing TG/state claim carrier at
begin, admission, final revalidation, and finish. It rejects a changed raw-TLS
binding and ordinary raw-NULL live stores. The same OS thread may legitimately
host a second Lua universe while raw TLS remains bound to the first, so the
existing runtime uses the main-TG fallback plus `tg_hint`; resume claims use the
same mechanism for suspended coroutines.

This is not yet an exact physical-actor proof. TG owner ids are logical ids,
and a different OS thread carrying an unrelated universe can obtain the same
main-TG fallback. It can therefore look reentrant to both the state-claim layer
and this guard. Likewise, the joined-world raw-NULL moved-close exception
proves the intended quiescent handoff in the focused fixture but does not by
itself distinguish two racing physical closers. This pre-existing VM ownership
gap is a b1.2.1 release blocker, not a supported exception.

The next carrier tranche must use a process-wide non-reused per-OS-thread actor
id and an atomic `(logical tid, actor)` state claim. Each TG also publishes its
actor. Second-universe fallback is then legal only for the same actor; worker,
attach, resume, GCSCAN/GCPREP, detach, and release paths must update the full
pair. Moved `lua_close` gets one explicit quiescent actor-transfer CAS which is
sticky through destruction. Arbitrary API-vs-close lifetime still needs the
dormant universe admission/close protocol; actor identity alone is not a heap
lifetime token.

An owner-local ACTIVE descriptor is detected before publication. The nested
guard pins global activation and retains its own leases/weak token without
touching the outer descriptor, so the outer ticket can still finish to IDLE.
Malformed publication or descriptor replacement pins global activation before
a retained guard can become store-authorized.

A foreign caller may pin global activation but cannot write owner-local guard
fields, finish the descriptor, release leases, or touch the owner's SSB. The
original owner remains responsible for cleanup. Registry shadow OOM falls back
to the exact legacy carrier plus absorbing NO_RECLAIM while retaining all local
leases. Registry release failure similarly leaves durable fail-closed
authority. Production keyed integration aborts on an unfinished stack-local
guard rather than discard the only representation of a live descriptor or
lease.

Custom `lua_Alloc` remains temporarily ignored/unsupported for the
internal-arena-only GC2 beta. This is temporary compatibility debt, not the
intended API contract. Restoring allocator callbacks requires the allocator
itself to participate in the lifetime/reclamation protocol; until then no
custom callback is invoked and no callback-owned allocation is assumed.

## Remaining b1.2.1 boundaries

This is a scalar publication cutover, not the end of table or FFI concurrency
work. The highest-priority remaining boundaries are:

- reader lifetime: public point readers (`lj_tab_getint`, `lj_tab_getstr`,
  `lj_tab_get`), iteration/NEXT, `#`/length, and direct VM/JIT AREF/HREF paths
  still export or consume raw vector slots outside bounded SMR/result leases.
  A helper cannot retroactively protect a `GCtab *` captured through a
  replaceable parent edge, so those callers also need parent-edge admission;
- migration completion: replace retry-until-root with the persistent MOVING or
  composite resize descriptor described above;
- new-key structure: hash-key claim/publication and collision-chain mutation
  need their own durable descriptor rather than relying only on the subsequent
  value CAS;
- range mutation: resize copy/freeze, `table.clear`, and TSETM structural range
  operations need explicit range transactions before typed root-gate close can
  become positive authority;
- recorder lifetime: JIT recording must not retain raw environment/cache table
  slots across allocation or yielding work; and
- physical actor and universe lifetime: close the fallback impersonation gap
  described above, then integrate exact universe admission before arbitrary
  racy API-vs-close can be called safe; and
- metadata/token arbitration for small-cell reuse, Huge free/realloc,
  quarantine, and eventual custom-allocator replacement.

No production closer yet enumerates and covers every `TGState.root_desc`.
Descriptor coverage and activation gates therefore remain conservative vetoes;
legacy close authority is intentionally retained.

## Evidence and performance checkpoint

The focused guard fixture covers by-value payloads, OPEN/PENDING admission,
CLOSING/COMMIT displacement to PENDING, LOST retry, committed versus failed
finish ordering, ACTIVE through forced-rescan INSTALLING, dirty-before-token,
nested fallback, two universes on one OS thread, suspended coroutine carriers,
foreign/NULL rejection, and joined-world raw-NULL close. A separate moved-state
fixture quiesces a universe, clears creator TLS, closes it on a fresh pthread,
and verifies finalizer writes to fresh array and hash tables. These tests do
not yet certify the missing physical-actor distinction described above.

The weak-clear cutover fixture publishes real replacement array and hash roots
between classification and the nil CAS. A non-OK clear now keeps the weak
cursor unconsumed and forces a fresh current-root pass; only replay on the
published successor may advance completion. The API handoff fixture deletes
all registry edges and forces full GC at both `luaL_newmetatable` winner paths.
Lookup and NIL-CAS observed results acquire exact referent leases before
release-publishing the enumerated winner root.

CLibrary close is now trace-safe: cache readers use OPEN/CLOSING admission,
lookups return rooted values, close is idempotent/nonwaiting, and escaped cdata
and recorded calls retain native handles until universe teardown. The deliberate
beta tradeoff is that explicit runtime close defers physical native-library
unload until universe close rather than risking an escaped/JIT use-after-unload.

The table CAS fixture covers all three semantic modes, array/hash and
cross-part resize states, stale-before-root versus success-after-root, and a
real committed CAS whose outward result becomes STALE while its dirty/rescan
handoff still completes. JIT/FFI suites exercise existing, previous-nil,
new-key, weak, metatable, trace-local, escaped, numeric, and GC array/hash
stores before and after active MT. Current focused passes include
`m3_gc2_table_store_guard`, `m3_gc2_weak_resize_retry`,
`m5_api_gc_handoffs`, `m5_tab_cas_store`, `m6_jit_table_store_helper`,
`m7_ffi_metatype`, and `m8_close_moved_state`. Strict GCC/Clang is refreshed
at the coherent commit boundary.

On one local active-MT microbenchmark of one million hot string-key integer
overwrites, the pre-lifetime-hardening checkpoint took about 0.445 CPU seconds
versus 0.123 seconds at `540c3de2` (about 3.6x). This is optimization debt, not
a performance claim. It is below the beta's catastrophic-slowdown threshold;
later b1.2.1 work should coalesce handoffs and eliminate redundant
weak/prepublication work without weakening the protocol. Primitive numeric
array stores continue to bypass the full guard.

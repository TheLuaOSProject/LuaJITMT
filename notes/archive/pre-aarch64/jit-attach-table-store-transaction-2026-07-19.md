# Prepared exact table store for `jit.attach`

Date: 2026-07-19

This b1.2.1 substrate adds a dormant prepared keyed table-store transaction for
the clocked `jit.attach` cut. It is not yet wired into `lib_jit.c`, the VM-event
reader, or production callback delivery. The existing attachment behavior is
unchanged until that separate integration lands. No file under `plan/` was
edited.

## Exact lifetime and ordering contract

The caller resolves and integerizes a candidate table-slot address while the
resolver still owns its vector lifetime and while allocation, resize help,
waiting and throws are legal. Parent, key, desired value and an explicitly
supplied expected value begin in ordinary semantic roots. Preparation then:

1. copies the Lua operands and rejects raw hidden/sentinel forms;
2. performs all semantic prepublication, weak and storage barriers before
   retaining any vector SMR reader;
3. enters a short GC2 SMR reader, fail-closed validates GC tag/header pairs,
   derives a fresh slot pointer from the current table roots, compares its
   integer address with the candidate, captures or checks the exact expected
   TValue, and takes a separate expected-object lease to prevent GC-address
   ABA;
4. leaves that short reader before store-guard admission or activation retry;
5. prepares, admits and revalidates the existing `LJGC2TableStoreGuard`; and
6. reacquires one nonwaiting SMR reader, revalidates current key and expected
   value, and returns PREPARED while retaining that final reader.

No pointer value survives the gap between the two readers. The candidate is a
`uintptr_t`; final validation derives fresh pointer provenance from a live
current table root and compares only integer addresses. It never evaluates or
dereferences a pointer from a retired allocation. The independent expected
lease prevents a reclaimed old GC object from passing the exact-value
comparison by same-address reuse. Cleanup clears the fresh pointer before its
final SMR lifetime ends.

After preparation, the caller may publish its attachment clock odd and invoke
the bounded commit. Commit has no `lua_State *`: it performs current key/value
validation, exactly one exact TValue CAS, and one post-CAS currentness check. It
does only bounded atomic/TLS validation of the exact owner and does not
allocate, wait, assist, safepoint, throw, run a barrier, or make a semantic
L-aware runtime callout. Production has no hook; `LJ_TAB_TEST_HELPERS` adds one
bounded non-L post-CAS root-swap hook solely for deterministic validation. Its
boolean result is commit authority and is separate from its slot status. In
particular, committed plus `STALE` means the CAS owns one attachment generation
even though the vector ceased to be current; the caller must publish even once
and finish the committed guard. A higher-level requested operation may begin a
new transaction only after a fresh semantic lookup; it must not treat the first
CAS as uncommitted.

Exact delete uses the captured expected TValue rather than an unconditional
nil store, so a concurrent handler replacement cannot be erased. Snapshot
replace similarly captures one exact previous value before clock admission.
Hidden table sentinels, FORWARD, finalizer claims, nil/NaN keys and mismatched
GC tag/header snapshots are rejected rather than becoming CAS operands.

After restoring the attachment clock even, finish/abort verifies the original
universe, TG, physical actor, exact `lua_State *` and state-owner claim before
touching the TLS-accounted SMR reader. It drops vector SMR before the
potentially longer GC handoff, while the guard and expected lease still retain
all semantic objects; then it publishes the committed dirty/rescan handoff and
releases the guard, expected lease and registry authority. Transactions are
linear and owner-only:
they cannot be copied or reinitialized while PREPARED, and every terminal or
failed attempt must be initialized before reuse.

## Deliberate limits

This primitive does not itself allocate a missing table key, resolve a stale
slot, manage the attachment clock, or call a VM-event handler. Those operations
remain caller-side and all retry/wait work must happen outside an odd clock
interval. In particular, future `jit.attach` wiring must add a held resolver
which derives and integerizes the candidate while its own vector authority is
still live; casting a naked `lj_tab_set()` result after return would reopen the
pointer-provenance race. That integration will pair the primitive with the
universe attachment clock and tri-state reader retry protocol.

The GC2 beta policy for custom `lua_Alloc` is unchanged: custom allocators are
still temporarily ignored while the internal-arena lifetime protocol is being
completed. This transaction neither widens that exception nor assumes a
callback-owned allocation.

## Focused evidence

`m5_tab_keyed_store_txn` covers array and production-shaped negative-integer
hash slots; snapshot replace; exact current commit and delete; changed and
FORWARD refusal; stale array and hash generation rejection; concurrent
replacement preservation; an actual same-TG resume claim on the wrong state;
exact SMR/root-descriptor cleanup; and deterministic
`{committed=true, status=STALE}` with one dirty/rescan handoff. The focused
fixture also passed a full `LUAJIT_DISABLE_JIT` library build with `-Werror`.

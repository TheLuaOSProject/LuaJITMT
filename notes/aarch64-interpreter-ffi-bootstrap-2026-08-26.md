# Apple ARM64 interpreter and FFI bootstrap (2026-08-26)

## Status and claim boundary

This checkpoint makes the fork build and run natively on Apple ARM64 through
an explicit internal bootstrap profile:

```sh
env MACOSX_DEPLOYMENT_TARGET=13.0 make -C src \
  XCFLAGS='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_DISABLE_JIT -DLUA_USE_ASSERT'
```

It is an interpreter/FFI bring-up checkpoint, not completion of the lockless
ARM64 port.  The default target gate still rejects Apple ARM64, and the
bootstrap gate requires GC64, macOS ARM64, and `LUAJIT_DISABLE_JIT`.  This is
intentional: the ARM VM still needs TG-local dispatch, VM-state ownership,
safepoint polling, and complete stack-dirty accounting before it can be called
a fully lockless multithreaded VM.  The JIT remains disabled and unported.

## Implemented in this checkpoint

### VM and bytecode compatibility

- The ARM protected-C-call entry is exported as `lj_vm_cpcall_asm`, matching
  the lockless C wrapper and the x86-64 VM ABI.
- The fork's `BC_CNEW`, `BC_CGET`, and `BC_CSET` local-cell bytecodes are
  implemented.  New cells and destination stack slots use the existing root
  publication helpers; closed-upvalue stores use the fork's release/barrier
  helpers.
- `UGET` and cell-backed `CGET` consume release-published values with ARM
  acquire loads.  Open-upvalue fallback writes are release-published and
  invalidate concurrent stack-root certifications.
- Closed `USETV`, `USETS`, `USETN`, and `USETP` stores route through the
  lockless upvalue publication helpers instead of the stock ARM write-barrier
  sequence.
- `lj_state_stack_pubtv` has assembler-visible linkage.  This is required by
  both the normal generated VM object and the amalgamated translation unit.

### Coroutine ownership

ARM `coroutine.resume` and `coroutine.wrap` now use the same state-ownership
claim/release protocol as the x86-64 path.  A failed validation releases an
acquired claim, a busy claim falls back without releasing someone else's
authority, and normal/yield/error returns preserve the result while releasing
the claim.

The tagged claim token lives in the outer VM frame scratch slot.  An earlier
version stored it in the ARM constant-base register; resuming a dead coroutine
then corrupted the caller's constant pool and crashed on the next `KSTR`.
`tests/t-threading-coroutine.lua` now permanently covers a failed dead resume
followed immediately by a constant load.

### FFI callback runtime and Darwin ABI

The ARM callback trampoline now:

- snapshots incoming `x0`-`x7` and `d0`-`d7` before calling helpers;
- acquires the current `CTState` and prepares a TG-local
  `CCallbackRuntime`;
- records the true incoming stack pointer, enters/leaves the native callback
  frame, and reloads scalar integer/floating results;
- returns zero for a dead/stale callback entry; and
- keeps the stack 16-byte aligned across helper calls and unwind paths.

Darwin AArch64 packs scalar callback arguments spilled after the register
banks by their actual size and natural alignment.  The callback decoder now
tracks the stack offset in bytes rather than indexing an `intptr_t` array.
Without this fix, a ten-`int` callback read argument ten from `SP+8` instead of
the ABI-correct `SP+4`.

The supported callback ABI claim remains exactly the set admitted by
`callback_checkfunc`: void returns, or enum/pointer/numeric scalar arguments
and results up to eight bytes.  Varargs, structs/unions, HFAs, vectors, complex
values, by-reference aggregates, and indirect `x8` returns are not supported.
The aggregate alignment clause in the decoder is defensive and does not claim
aggregate callback support.

## Validation on the native host

The following passed with Apple clang 21 and a macOS 13 deployment target:

- clean normal ARM64 bootstrap build and ARM64 Mach-O inspection;
- clean amalgamated bootstrap build;
- no undefined compiler atomic helper in the executable;
- vendored stock interpreter suite: 387/387;
- closure, protected-call, and dead-coroutine/next-constant smoke tests;
- cross-thread coroutine handoff, including repeated normal and `-joff` runs;
- the Lua threading API and state-owner fixtures;
- concurrent FFI callback runtime at 1 x 80, 4 x 220, and 8 x 500 rounds;
- compiler-generated ten-integer callbacks on attached and TLS-less pthreads;
- nested callbacks, callback owner lifetime, stop requests, automatic attach
  and detach, and C++ exception unwind cleanup;
- stacked float/double arguments, mixed narrow/integer arguments, scalar
  integer/floating/pointer results, and the stock callback suite.

The canonical Lua test wrapper does not yet carry this opt-in XCFLAGS profile
through its automatic clean rebuild.  Bootstrap validation therefore uses the
exact clean build above and direct test/fixture invocations; an unqualified
wrapper rebuild correctly hits the default ARM target rejection.

## Known gaps and next work

The following are blockers for a fully lockless ARM VM claim:

1. ARM still dispatches through the universe-global `GG_G2DISP` table, writes
   global `cur_L`/`vmstate`, and updates global hook counters.  It must carry a
   stable TG dispatch base like x86-64.
2. ARM bytecode backedges, returns, native entry/exit edges, and long-running
   helpers do not yet implement the TG safepoint request/acknowledgement
   protocol.  This is consistent with the observed intermittent
   `t-threading-hooks.lua` failure to redispatch every live worker.  On the
   final assert-enabled build, four repeated runs passed and the fifth failed
   at the positive "hook did not reach every live worker" assertion.
3. Ordinary ARM stack writes still need a complete audit and TG
   `stack_dirty_epoch` parity with x86-64.  The local-cell/upvalue paths fixed
   here are only the first explicit subset.
4. The ARM64 JIT backend, trace lifecycle, XPOLL lowering, executable-memory
   publication, and instruction-cache synchronization remain unported.
5. PAUTH/BTI and arm64e callback entry variants were not exercised on this
   host.
6. As on the inherited x86 callback path, automatic detach drops `mt_live`
   before assembly reloads the TG callback result.  No reproduction was found,
   but a later hardening should retain explicit lifetime authority through the
   final result reload.

The first root-publication worklist is concrete:

- `vmeta_tgetv` must not reuse `BASE` after a helper that can relocate the
  stack or overwrite a helper-published result with an untracked plain store;
  `vmeta_tsetv` has the same stale-`BASE` hazard after its helper.
- `cont_ra` and terminal `cont_cat` need the x86 sequence of result store,
  dirty-epoch invalidation, and conditional `lj_gc_pubtvroot_vm` publication.
- `TGETV`, `TGETS`, `TGETB`, `TGETR`, and `ITERN` still traverse naked table
  storage without the x86 acquire/generation protocol.  The conservative ARM
  bootstrap should use rooted table helpers, reload `BASE`, and publish the
  destination before any inline fast path is considered.
- Call/return topology, `fff_res`, test-copy/MOV/CAT/constants, FNEW/TNEW/TDUP,
  ITERC, VARG, the RET families, and IFUNCV still lack the corresponding x86
  dirty-epoch sites.

For cold single-slot paths, `lj_state_stack_pubtv(L, L, slot)` is the safest
first implementation because it combines a release self-store, dirty-epoch
invalidation, and root notification.  Hot paths can later materialize the
slot address for `stlr`, use a lightweight TG dirty helper, and conditionally
publish new collectable roots.  Owner-private stack reads do not all need
`ldar`; acquire is required at shared cells and other externally published
edges.

`lj_state_stack_dirty_vm(L)` is the bounded substrate for those hot paths.  It
selects the state's owning TG with `L2TG(L)` and performs exactly one relaxed
atomic increment of `stack_dirty_epoch`.  It cannot allocate, enter GC, wait,
or touch a TValue, so it is safe at an owner-authorized VM call boundary.  The
relaxed increment invalidates only the TG's stack-scan certification: the ARM
caller must still release-publish the slot (for example with `stlr`) before the
invalidation and must call `lj_gc_pubtvroot_vm` when a new collectable root
needs notification.

TG-local ARM dispatch and VM-state publication have now landed.  The next
implementation slice is complete stack-dirty/result-root publication,
followed by safepoint polling and root-scan certification.  Only after that
gate passes should the bootstrap macro become
a supported ARM interpreter target or JIT work begin.

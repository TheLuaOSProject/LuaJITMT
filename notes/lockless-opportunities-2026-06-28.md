# Lockless Opportunities - 2026-06-28

## Position

More lockless is useful only when it preserves Lua semantics and makes shared
runtime state easier to reason about. The project should continue to prefer
safety, stability, and language behavior over LuaJIT performance parity.

## Best Next Targets

1. Traced FFI call native-state protocol.
   Ordinary traced FFI calls are currently disabled with
   `LJ_FFI_RECORD_CALLS=0`. Before re-enabling them, `IR_CALLXS` needs the same
   native enter/leave protocol as interpreted `lj_ccall()`. This is worth doing
   only as a staged x64 slice with explicit result-preservation and STOPREQ
   tests; direct traced foreign calls must stay disabled until then.

2. Table resize follow-up stress/proofs.
   Resize forwarding is now production-shaped, but it is still one of the
   riskiest shared objects because it combines replacement generation
   publication, weak clearing, GC traversal, JIT fast paths, and VM stores.
   More stress, table-forwarding probes, generated dump/ASM checks where the
   invariant is emitted code, and design notes are worth it. Repository
   source-search guards are not. Removing the transient `KEYLOCK`/`FORWARD`
   waits is not the goal; bounding and proving them is.

3. C-closure upvalue and direct API mutation surfaces.
   This is worth a focused audit now that the closed Lua-upvalue path is sealed,
   because it is a smaller but still user-visible mutation channel.

4. FFI layout/read fast paths that still fall back to `lj_ctype_parse_lock()`.
   Do this only for stable read-only queries where snapshot helpers can prove
   the C type table did not grow or roll back underneath the reader. Keep actual
   parser mutation serialized.

5. Dispatch/JIT coordination polishing.
   `lj_dispatch_update()` still has a global update token before syncing
   per-TG dispatch tables. This is probably not a hot-path problem, but a
   future per-TG lazy-update cleanup may simplify hook/profiler interactions.

## Recently Closed

- X64 JIT closed-upvalue stores now route every `IR_USTORE` into `IR_UREFC`
  through `lj_func_storeuv_forjit()`, including numeric and primitive values.
- `lua_getlocal()` now acquire-loads closed local-cell upvalues, matching the
  existing `lua_getupvalue()` publication discipline.

## Usually Not Worth Making More Lockless

- `ffi.cdef` and mutable C type graph mutation.
  Keep this serialized. It is high-risk, hard to reason about, and not worth
  weakening safety unless a specific workload proves it matters.

- `threading.mutex`.
  This is a user-facing synchronization primitive. Making it "more lockless"
  would change the purpose of the API, not improve the runtime.

- Channel receive/send waits and thread join waits.
  These are blocking semantics by design. They should keep correct native-state
  transitions, not try to be wait-free.

- GC2 parked-worker lifecycle changes.
  `gc2_worker_control` serializes worker start/stop and parked-pool lifecycle.
  It is rare, semantic coordination. Replacing it with a more complicated
  lock-free protocol would likely reduce stability.

- Safepoint handshake leadership and full trace flush leadership.
  These are global coordination points. CAS leader tokens and futex waits are
  acceptable if they keep the protocol clear and bounded.

- GDBJIT descriptor locking.
  This protects debugger-facing descriptor publication, not a hot language path.

- Legacy explicit-GC exclusion while secondary threads or finalizer-spawn
  transitions are active.
  This is a semantic safety gate around legacy GC APIs, not a normal allocator
  or interpreter lock. It should be narrowed only with a concrete proof that the
  legacy operation can observe a concurrent-safe root set.

## Current Lock/Wait Inventory Outside `ffi.cdef`

- `src/lib_threading.c`: `threading.mutex` uses CAS plus futex wait/wake. Good
  reason: explicit user synchronization API.
- `src/lj_chan.c` and `src/lib_threading.c`: channels and joins use futex waits.
  Good reason: blocking language/runtime operations.
- `src/lj_gc2.c`: `gc2_worker_control_lock_l()` serializes parked worker
  lifecycle. Good reason: shutdown/startup correctness.
- `src/lj_safepoint.c`: handshake leader CAS plus futex waits. Good reason:
  global safepoint/trace-flush coordination.
- `src/lj_gdbjit.c`: CAS lock around GDBJIT descriptor publication. Good reason:
  external debugger integration.
- `src/lj_ctype.c` and FFI-related callers: `lj_ctype_parse_lock()` is still
  used outside direct `ffi.cdef` for layout/read fences and rollback safety.
  This is tied to the mutable C type graph and should remain conservative.
- Table, string table, cdata finalizer registration, dispatch update, parser
  keepalive, serialization dictionary, and recorder-template waits are mostly
  transient CAS-claim waits. These are lockless publication protocols with a
  bounded yield path, not traditional mutexes. Remove individual waits only when
  the corresponding reader can prove it will never observe half-published state.

## Recommendation

Next real lockless progress should be the traced FFI native-state bridge or
additional table-resize proof/stress work. Keep lifecycle, explicit blocking
APIs, `ffi.cdef`, debugger descriptors, and legacy-GC exclusion simple. If a
path is rare and semantic coordination-heavy, the lockless version must be
clearly simpler or clearly safer; otherwise it is not a good trade.

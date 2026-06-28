# Lockless Opportunities - 2026-06-28

## Position

More lockless is useful only when it preserves Lua semantics and makes shared
runtime state easier to reason about. The project should continue to prefer
safety, stability, and language behavior over LuaJIT performance parity.

## Best Next Targets

1. X64 JIT closed-upvalue stores.
   Current audit result: GC-valued `IR_USTORE` into `IR_UREFC` already routes
   through `lj_func_storeuv_forjit()`, but numeric and primitive stores still
   use raw/split stores in the x64 backend. This is worth fixing because closed
   upvalues are core Lua semantics and the helper path already exists.

2. `lua_getlocal()` local-cell acquire reads.
   The debug/API write side publishes with release semantics, while the local
   cell read branch still needs the same acquire load style already used by
   `lua_getupvalue()`. This is a small, high-confidence stability improvement.

3. Traced FFI call native-state protocol.
   Ordinary traced FFI calls are currently disabled with
   `LJ_FFI_RECORD_CALLS=0`. Before re-enabling them, `IR_CALLXS` needs the same
   native enter/leave protocol as interpreted `lj_ccall()`. This is worth doing
   behind the disabled flag, starting with x64 cdecl scalar/pointer returns.

4. Table resize follow-up stress/proofs.
   Resize forwarding is now production-shaped, but it is still one of the
   riskiest shared objects because it combines replacement generation
   publication, weak clearing, GC traversal, JIT fast paths, and VM stores. More
   stress and source guards are worth it.

5. C-closure upvalue and direct API mutation surfaces.
   This is probably worth a focused audit after the closed Lua-upvalue path is
   sealed, because it is a smaller but still user-visible mutation channel.

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

Do the upvalue/JIT helper slice next, then the traced FFI native-state bridge.
Keep lifecycle and explicit blocking APIs simple. If a path is rare and semantic
coordination-heavy, the lockless version must be clearly simpler or clearly
safer; otherwise it is not a good trade.

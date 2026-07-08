# FFI Shared Cdata Hammer

The M7 FFI target includes user-racy shared cdata field access. LuaJIT cannot
make C payload races deterministic, but the VM must stay safe: field loads and
stores must not corrupt ctype metadata, produce internal sentinels, crash during
GC, or make the cdata unusable after racing workers detach.

`t-ffi-cdata-shared-hammer.lua` shares one struct cdata object across spawned
Lua threads and hammers aligned integer fields, array elements, `_Bool`, boolean
bitfields, small integer bitfields, and a nested struct. Its assertions are
domain checks rather than last-writer checks because the user payload writes are
intentionally racy. The same script runs under interpreter and default-JIT modes.

This is runtime coverage only; it proves the VM safety contract under racy user
payload access.

2026-07-08 active-MT recorder gate and shutdown root fix:
- Repeated `t-ffi-cdata-shared-hammer.lua` runs exposed two separate issues:
  active-MT JIT recording of shared cdata field access, and a shutdown crash
  where stale duplicate root-spine entries could redispatch a freed child
  `lua_State`.
- `recff_cdata_index()` now aborts recording existing runtime cdata while more
  than one VM thread is live, so racy shared cdata field loads/stores use the
  interpreter path until the traced CLOAD/CSTORE root and aliasing protocol is
  made MT-safe. Cdata allocated inside the trace keeps the stock CNEW/CNEWI
  field fast path.
- Thread-state frees now use the deferred GC-body free path and legacy sweep
  rejects stale thread headers whose `glref` no longer names the active global
  state.
- Single-threaded FFI cdata indexing remains recorded, preserving the stock
  fast path outside active lockless threading.
- `t-ffi-cdata-shared-hammer.lua` now asserts the policy: a single-thread field
  load/store loop records a trace, the same loop against existing cdata with a
  live worker TG records no traces, and trace-owned `ffi.new()` cdata still
  records before the shared-cdata hammer starts.

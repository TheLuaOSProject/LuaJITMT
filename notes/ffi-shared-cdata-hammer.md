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

2026-07-08 GC2 weak-table traversal fix:
- The hammer also exposed a GC2 crash while worker-triggered collection was
  reading `__mode` from a weak/self-metatable table before validating the table's
  current node generation. GC2 now snapshots table storage before weak-mode
  lookup, matching the legacy collector order, and validates the metatable object
  before using the fast metamethod cache.

2026-07-10 worker-generation recorder fix:
- `recff_cdata_index()` used the instantaneous GC2 thread count to decide
  whether an existing cdata field load/store could be recorded. After the first
  worker exited, that count returned to one while the one-way `mt_active` latch
  stayed set. A shared-cdata trace recorded in this gap was not covered by the
  first-activation flush and could run during the next worker generation.
- The recorder now uses `lj_record_mt_runtime_shared()`, matching table/frame
  recording policy and preserving the trace-owned `CNEW`/`CNEWI` exception.
  Existing cdata therefore stays interpreted after any threading activation
  until traced CLOAD/CSTORE has a complete shared-root and aliasing protocol.
- The hammer now records again after joining its first worker and requires the
  sticky generation-safe gate before it starts the later multi-worker phase.

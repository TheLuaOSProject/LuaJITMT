# Progress Report - 2026-06-28 Threading CI Behavior

Scope: x86_64/Linux `v2.1` threading behavior and CI coverage.

## Landed In This Area

- `m4_threading_capi` no longer relies on pre-existing STOPREQ state for its
  blocking-operation checks.
- Join and mutex blocking behavior is covered by fixtures that publish a fresh
  STOPREQ only after the waiting operation has entered native wait.
- The C API attach-order and shutdown behavior are covered by runtime fixtures.
- Blocking threading cases have bounded timeouts so hangs become diagnostics.

## Current Threading Coordination

Kept intentionally:

- `threading.mutex` as the explicit user synchronization primitive.
- Channel send/receive and thread join waits for blocking API semantics plus
  STOPREQ/native-state delivery.
- Per-`lua_State` owner claims to prevent concurrent coroutine mutation.
- Safepoint leadership for global GC/shutdown/redispatch/trace-flush
  coordination.

Worth improving:

- More contention behavior around table claim waits.
- More stress around GC2 active-thread collect, parked workers, and finalizer
  interactions.
- Release smoke across Linux, Wine/Windows, and Darling/macOS before publishing.

## Verification At The Time

- `git diff --check`
- Focused debug C fixture for threading C API.
- `tools/ci/lua_test.sh m4_threading_shutdown`
- `tools/ci/lua_test.sh m4_threading_upvalue`
- `tools/ci/lua_test.sh m5_upvalue_publish_gc`

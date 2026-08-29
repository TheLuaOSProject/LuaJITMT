# Threading mutex STOPREQ wake

- `threading.mutex:lock()` now waits in bounded native futex slices instead of
  one indefinite futex wait. This preserves the public blocking mutex API while
  ensuring a remotely acknowledged `LJ_GC2_HS_STOPREQ` is delivered when the
  waiter next leaves native state, even if the mutex owner has not unlocked.
- `tests/t-safepoint-handshake.c` covers the behavior with a worker blocked in
  `mutex:lock()`: after a global STOPREQ handshake, the worker must report the
  VM-shutdown interruption before the main thread unlocks the mutex.
- The M4 threading notes document why we avoid reintroducing the indefinite mutex futex wait.

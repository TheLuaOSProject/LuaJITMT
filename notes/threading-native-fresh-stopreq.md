2026-06-27

- Routed public threading native waits through a fresh STOPREQ helper:
  `threading.sleep()`, `threading.mutex:lock()`, and `threading.thread:join()`.
- `thread:join()` keeps its cleanup ordering: a STOPREQ observed during
  `pthread_join()` is checked after result claiming and live-thread cleanup, but
  a pre-existing sticky shutdown flag no longer interrupts an otherwise
  successful join.
- Added sticky regressions for `threading.sleep()`, delayed `mutex:lock()`, and
  `thread:join()`.
- Added an M3 source guard rejecting raw threading wait STOPREQ checks outside
  the fresh helper.

2026-06-27

- Finished routing `lib_io.c` native stdio STOPREQ checks through the fresh
  STOPREQ helper instead of checking every stale sticky shutdown flag after
  `lj_native_leave()`.
- Covered prompt cleanup style paths that need cleanup before throwing:
  `io.open()`/`io.input()` cleanup still closes a freshly opened file before a
  fresh STOPREQ is delivered, and `io.popen()` / `io.tmpfile()` keep the same
  cleanup-before-throw behavior.
- Added a pending STOPREQ poll in the fresh helper so quick stdio calls that
  leave with a request still pending deliver shutdown without re-enabling
  stale sticky-only interruptions.
- Added sticky regressions for regular file operations (`read`, `write`,
  `flush`, `seek`, `setvbuf`, `close`) plus `io.open()` and `io.popen()`.
- Replaced pre-published regular-file read probes with blocking `io.popen()`
  read probes, so fresh STOPREQ is published while the read is in native state.
- Helper comments document why `lib_io.c` native leaves must use the fresh
  STOPREQ helper. The old implementation-text assertion rejecting raw checks is obsolete.

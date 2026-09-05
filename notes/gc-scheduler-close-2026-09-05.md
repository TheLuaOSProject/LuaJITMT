# Scheduler fixture performs its real close

The scheduler's terminal-TG subtest left its synthetic shutdown flag set before
calling `lua_close`. The real close correctly rejected that state, leaving the
second test universe allocated. ASan reported 131,280 leaked bytes in six
allocations, and a read-only debugger witness confirmed the rejected call.

The fixture now restores only its synthetic flag after all terminal-unlink
assertions and before real close admission. Runtime guards remain intact.
A separate witness observes successful admission, ordinary shutdown, GC2
teardown and return to the original TLS/root checks.

Eight isolated full runs pass, including four with ASan and LeakSanitizer.
The registered scheduler suite also passes all three components on runtime
`eb8a5b2f`. Original leaks, rejected-close observations and failed fixture-header
compile attempts remain preserved; debugger observations are not counted as
full test passes.

See the [root review](evidence/gc-scheduler-close-2026-09-05/root/review.md),
[owner handoff](evidence/gc-scheduler-close-2026-09-05/owner/HANDOFF.md),
[canonical results](evidence/gc-scheduler-close-2026-09-05/root/canonical.json),
and [archive manifest](evidence/gc-scheduler-close-2026-09-05/manifest.json).
This repair addresses the fixture leak, without a general leak-freedom or
release-readiness claim.

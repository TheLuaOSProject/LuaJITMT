# Source Guard Policy - 2026-07-05

Current tree policy: tests must not read production source files to assert that
an implementation uses or avoids a particular spelling. Lockless invariants
belong in behavior tests, focused counters exposed by test-helper builds,
comments beside the constrained code, and notes for cross-file rationale.

This matters because source-text checks make cleanup and stock-style rewrites
fragile without proving VM behavior. They also create false confidence: a test
can match a token while the real fast path, memory order, or fallback behavior
has changed.

Allowed file reads in the harness are artifact reads: logs, generated package
metadata, benchmark CSVs, bytecode round-trip payloads, and release archives.
Release scripts may grep build-info files inside archives because those files
are public artifacts. That is not a source guard.

DynASM `.byte` strings in VM backend files are also not instruction raw-byte
patches. The current x86/x64 matches are unwind/debug-frame metadata emitted
through the existing DynASM frontend path. Replacing those requires frontend
support for the relevant CFI encodings, not a test that bans the spelling.

The current audit did not find remaining source-text assertions in `tests/` or
`tools/`. The shared `suite_utils.read_file()` comment records the harness-side
rule so new tests keep the same boundary.

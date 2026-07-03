# Public Output Assertions

Active tests should assert observable behavior, public CLI output, structured
archives, or runtime counters.

The Lua suite helpers now expose this distinction directly:

- `assert_public_text_contains()`
- `assert_public_text_all_contains()`
- `assert_public_text_any_contains()`
- `assert_public_text_count()`
- `public_text_lines()`

These helpers are for command output and other public text produced by a test
scenario, such as benchmark rows or expected error reporting. When
implementation details matter, document the rationale beside the code and
prefer a C fixture, runtime counter, semantic check, stress test, or stock-suite
comparison for observable behavior.

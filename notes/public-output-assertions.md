# Output Assertions

Active tests should assert observable behavior, public CLI output, structured
archives, or runtime counters.

The Lua suite helpers now expose this distinction directly:

- `assert_output_contains()`
- `assert_output_all_contains()`
- `assert_output_any_contains()`
- `assert_output_count()`
- `output_lines()`

These helpers are for command output and other artifacts produced by a test
scenario, such as benchmark rows, manifests, release metadata, or expected error
reporting. Tests must not parse repository implementation source as a proxy for
behavior. When implementation details matter, document the rationale beside the
code and prefer a C fixture, runtime counter, semantic check, stress test, or
stock-suite comparison for observable behavior.

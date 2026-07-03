# Static invariant audit, 2026-07-03

Tracked active tests, CI, and release workflows must not pass or fail by
matching repository source text, generated IR/ASM text, objdump output,
generated encoding details, helper-name lists, raw-field spelling, or serialized
bytecode spelling. Those artifacts are implementation spelling, not the
contract.

Current audit result:

- No active or tracked-disabled executable text-matching invariant suite remains.
- `tools/ci` contains the Lua suite launcher and platform build smoke script.
- GitHub CI routes through platform builds and Lua behavior suites.
- `tests/lib/suite_assert.lua` exposes public-artifact text helpers only.
- `jit.util.traceir()`, `tracemc()`, and `traceexitstub()` appear in the active
  flush-race test only as no-crash reader probes under concurrent trace flush.
  The test does not assert IR or mcode spelling.
- Bytecode coverage loads and executes opaque dumps; it does not compare dump
  spelling.
- Release verification greps install metadata and archive manifests, which are
  public product artifacts.

The durable replacement for brittle text-matching checks is:

- code-adjacent comments for ordering, ownership, nonblocking, and ABI rules;
- notes when the rationale spans files or subsystems;
- Lua behavior tests, C race/lifetime fixtures, stock-suite coverage,
  benchmarks, or release-artifact checks when the invariant has an observable
  failure mode.

Historical notes may mention that a source or dump text check once existed only
as audit history. They are not dormant tests to restore. If an old wrapper
described a real invariant, keep the reason beside the constrained code and
cover the observable failure through the current harness.

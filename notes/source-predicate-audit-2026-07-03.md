# Repository-source predicate audit, 2026-07-03

Current policy: tests and CI must not decide pass/fail by reading repository
source files and matching helper names, field accesses, calls, raw-byte
snippets, or other implementation spelling. The invariant still matters, but
the durable record belongs beside the constrained code and in notes that explain
the ownership, ordering, nonblocking, or ABI reason.

Coverage should use behavior, C race/lifetime fixtures, generated artifacts,
benchmark output, or release packaging output. Generated JIT/bytecode/mcode/ASM
dumps are still valid artifacts to inspect when the generated output is the
behavior under test.

Audit result: active `tests/`, `tools/ci`, and GitHub workflow checks do not
open `src/` or DynASM source files to make source-text assertions. Remaining
file reads in the Lua harness cover generated dumps, captured process output,
temporary marker files, benchmark CSVs, and release/build artifacts. Shell
searches in release tooling inspect install metadata, object/generated output,
or packaging manifests, not implementation source snippets.

Historical milestone wrapper suites are not exempt. If an old wrapper described
a real invariant, keep the reason as code-adjacent comments and notes, then
cover the observable part through the current harness.

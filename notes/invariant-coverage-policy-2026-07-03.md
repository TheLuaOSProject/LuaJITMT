# Invariant coverage policy, 2026-07-03

Current policy: tests and CI must decide pass/fail through behavior,
generated artifacts, benchmark data, or release packaging. There must be no
repository implementation-text checks: no source matching, helper-name
inventories, raw-field spelling checks, or implementation-shape grep rules as
pass/fail criteria. The invariant still matters, but the durable record belongs
beside the constrained code and in notes that explain the ownership, ordering,
nonblocking, or ABI reason.

Coverage should use behavior, C race/lifetime fixtures, generated artifacts,
benchmark output, or release packaging output. Generated JIT/bytecode/mcode/ASM
dumps are still valid artifacts to inspect when the generated output is the
behavior under test.

Audit result: active `tests/`, `tools/ci`, and GitHub workflow checks use
generated dumps, captured process output, temporary marker files, benchmark
CSVs, and release/build artifacts. Shell searches in release tooling inspect
install metadata, object/generated output, or packaging manifests, not
repository implementation text.

Historical milestone wrapper suites are not exempt. If an old wrapper described
a real invariant, delete the source-text check, keep the reason as
code-adjacent comments and notes, then cover the observable part through the
current harness.

Manual implementation searches remain useful while developing or reviewing a
slice, but their result is an engineering observation, not a CI contract. A
release or regression test should fail on wrong behavior, wrong generated
output, a broken artifact, or a benchmark regression. If a constrained source
shape is required, explain why in the code comment and supporting note instead
of pinning the spelling in a test.

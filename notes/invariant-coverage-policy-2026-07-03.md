# Invariant coverage policy, 2026-07-03

Current policy: tests and CI must decide pass/fail through behavior,
generated artifacts, benchmark data, or release packaging. There is no active
or desired repository text-matching suite. The invariant still matters, but the
durable record belongs beside the constrained code and in notes that explain
the ownership, ordering, nonblocking, or ABI reason.

Coverage should use behavior, C race/lifetime fixtures, generated artifacts,
benchmark output, or release packaging output. Generated JIT/bytecode/mcode/ASM
dumps are still valid artifacts to inspect when the generated output is the
behavior under test.

Audit result: active `tests/`, `tools/ci`, and GitHub workflow checks use
generated dumps, captured process output, temporary marker files, benchmark
CSVs, and release/build artifacts. Shell searches in release tooling inspect
install metadata, object/generated output, or packaging manifests.

Historical milestone wrapper suites are not exempt. If an old wrapper described
a real invariant, keep the reason as code-adjacent comments and notes, then
cover the observable part through the current harness.

Manual implementation searches remain useful while developing or reviewing a
slice, but their result is an engineering observation, not a CI contract. A
release or regression test should fail on wrong behavior, wrong generated
output, a broken artifact, or a benchmark regression.

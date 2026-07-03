# Invariant Coverage Audit

Date: 2026-07-03

Policy: the test harness, CI, and release workflows must not fail based on
repository implementation text. That includes helper-name inventories,
raw-field spelling checks, broad implementation-shape grep rules, and old
milestone wrapper source inventories.

Why: implementation spelling is not the contract. The contracts in this fork
are memory ordering, ownership, publication, native-state boundaries,
generated code shape, benchmark behavior, and release artifact contents.
Pinning implementation spelling makes refactors brittle while still missing
semantic regressions.

Replacement: keep the rationale beside the constrained code as a short comment,
and add a note when the reason spans multiple files. Cover observable effects
through Lua behavior tests, C race/lifetime fixtures, generated JIT/bytecode/
mcode/ASM artifacts, benchmark comparisons, or release packaging checks.

Current audit: active tests and CI read generated dumps, captured process
output, temporary files, benchmark CSVs, build outputs, and release manifests.
No active suite should read `src/*` implementation text for pass/fail. Manual
`rg`/`grep` searches remain review tools only, never gates.

# Invariant Coverage Audit

Date: 2026-07-03

Policy: the test harness, CI, and release workflows must not fail based on
repository implementation text. That includes helper-name inventories,
raw-field spelling checks, broad implementation-shape grep rules, and old
milestone wrapper source inventories.

Why: implementation spelling is not the contract. The contracts in this fork
are memory ordering, ownership, publication, native-state boundaries, runtime
behavior, benchmark behavior, and release artifact contents. Pinning
implementation spelling makes refactors brittle while still missing semantic
regressions.

Replacement: keep the rationale beside the constrained code as a short comment,
and add a note when the reason spans multiple files. Cover observable effects
through Lua behavior tests, C race/lifetime fixtures, public API/ABI checks,
bytecode dump/load execution, benchmark comparisons, or release packaging
checks.

Current audit: active tests and CI should read captured process output,
temporary files, benchmark CSVs, build outputs, and release manifests. Bytecode
dumps may be loaded and executed as opaque artifacts, but exact dump bytes are
not a pass/fail contract. No suite should read repository implementation text,
generated IR/ASM/mcode text, or machine-code byte patterns for pass/fail, and
there is no exception for older milestone wrappers, release preparation, or
local-only scripts. If such a check is found, delete it and preserve only the
reason as code-adjacent documentation plus observable coverage where possible.

# No Source Guards Audit, 2026-07-03

Active tests, CI, and release automation must not fail based on repository
implementation text, helper names, raw-field spelling, generated IR/ASM text,
objdump output, or mcode byte patterns. This applies to the historical milestone
wrapper suite too; old source inventories are not dormant tests to restore.

The durable form for these invariants is:

- a code-adjacent comment explaining the ordering, ownership, nonblocking, or
  ABI reason;
- a note when the reason spans files or subsystems;
- behavior, C fixture, public API/ABI, benchmark, stock-suite, or release
  artifact coverage when the invariant has an observable failure mode.

Artifact text checks remain valid only for artifacts that are the product under
test: captured process output, benchmark CSVs, install manifests, release
metadata, and opaque bytecode dump/load files that are executed rather than
byte-compared. `jit.util.traceinfo()` checks remain valid when the contract is
traceability of a workload. Generated compiler text and repository source text
remain review tools only, not CI gates.

Current audit result: `tools/ci` contains only the Lua test launcher and
platform build smoke script; GitHub CI builds/runs platform binaries and does
not grep repository source; Lua suite file reads target temporary outputs,
benchmark CSVs, bytecode/load artifacts, and release/build artifacts. Historical
notes that mention old grep/source/dump checks are audit history, not a backlog.
The active Lua assertion helper layer no longer exposes generic source-text or
pattern-match assertions; the remaining exact/plain checks are artifact-named
and are for public artifacts and captured process output.

# Invariant Documentation And Observable Coverage

Historical source-text pass/fail checks are retired for the whole repository,
including old milestone wrappers and any disabled, local-only, or release-only
variants. Active tests, CI, and release automation should not decide pass/fail
from repository implementation spelling, helper-name inventories, generated
IR/ASM text, objdump output, serialized bytecode spelling, or generated mcode
encoding checks.

The replacement is documentation plus observable coverage:

- Put the reason beside the constrained implementation when an access pattern,
  helper surface, ordering edge, native-state boundary, or ABI shape is required.
- Add a note when the reason spans more than one subsystem.
- Cover the observable failure mode with Lua behavior tests, C race/lifetime
  fixtures, stock-suite coverage, runtime counters, benchmark comparisons, or
  release/package artifact checks.

Artifact text checks remain valid when the artifact itself is the product under
test: captured process output, benchmark CSVs, install manifests, release
metadata, and opaque bytecode dumps that are loaded and executed. That keeps the
suite focused on stock Lua/LuaJIT semantics, lockless publication and lifetime
behavior, platform build/run health, and release deliverables instead of source
layout.

Current audit result:

- `tools/ci` contains the Lua launcher and platform build smoke harness.
- GitHub CI builds and runs each platform target; it does not inspect
  repository source for implementation shape.
- The Lua harness exposes `Test:read()` as an artifact reader for files produced
  or consumed by tests.
- Release verification reads archive metadata, install manifests, checksums,
  and smoke output because those are published artifacts.

If an old wrapper described a real requirement, keep the requirement as a
comment and note. Do not keep the wrapper as a pass/fail check.

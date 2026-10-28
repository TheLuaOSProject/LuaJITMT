# Math helpers, dynamic bytes and early-return checkpoint

Production source `60d304989` on `codex/arm64-parity` combines:

- Native pure libm helpers, including loop-carried results and independent
  allocated-register/spill/snapshot-rename survival checks across calls.
- Positive dynamic `string.byte` indices with exact source argument, same-
  string length, conversion and bounds proofs.
- Multi-operation leaf branches and bounded early-return branches with exact
  source return/result binding and independent snapshot replay.
- The suite runner's intended engine-wide `jit.off()` behavior.

The complete `arm64_jit_fail_closed_gate.sh` passed on that source, including
the four ARM64/arm64e assertion/release M7 FFI profiles. Each leaf profile
reports 97 cases, 402 native invocations, 96 terminal-return exits, 306 same-
trace reuses and 97 GC cases. The early-return case has a separate forced
comparison-edge exit; it is not counted as an additional terminal-return exit.
The malformed leaf fixtures report 1,579 semantic and 1,589 allocated rejects.

Ordinary ARM64 and signed x64 each pass both 509-case stock corpora (source and
imported bytecode). ARM64 also passes thirteen ordinary Lua companions; x64
passes return/compound, math and string companions. Signing, inline-atomic
linkage and selected ordinary-build test-hook exclusions were checked. The
build was restored to ordinary ARM64 assertion mode afterward.

The twenty-workload inventory passes all 260 result comparisons on each
architecture. ARM64 now has selected-root exit witnesses in fifteen rows,
versus eighteen on x64. Early-return branching is the new ARM64 row. Captured
nested calls, Lua varargs and multiple results remain sampled differences;
iterator and I64 rows remain inconclusive on both targets. This is not full
compilation, performance or x86_64 parity evidence.

Follow-up `15175bb3d` binds dynamic-byte allocated views to their exact source
prototype/bytecode/owner. Its separate four-profile string-byte contract passes
including the wrong-prototype rejection. The complete experimental gate and
ordinary cross-architecture matrix above were run before this tightening.

Evidence is under
`/Users/frityet/Scratch/ljmt-math-dynamic-early-20261028/`, including
`experimental.log`, `ordinary-matrix.log`, both architecture stock-corpus logs,
both inventory TSVs, and `prototype-bound-byte-four-profile.log`.

This checkpoint is pushed on the port branch. `v2.1` remains at `49523abd`:
the earlier JIT-enabled harness table-overflow/TDUP crash is still unresolved.
The corrected harness passing does not establish a runtime fix. See the
[investigation](arm64-harness-tdup-investigation-2026-10-28.md),
[math details](arm64-pure-math-helpers-2026-10-28.md),
[byte details](arm64-string-byte-contract-2026-10-28.md), and
[early-return details](arm64-jit-lua-early-return-branches-2026-10-28.md).

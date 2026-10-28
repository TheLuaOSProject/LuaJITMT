# ARM64 primitive-leaf, scalar-math and string-length checkpoint

Source through `ba6d4da3`, plus the dynamic-input inventory at `14619e46`, is
integrated into `v2.1`. The checkpoint adds nil/false/true locals to bounded
pure Lua leaves, closes the nonterminal child-to-caller result-binding gap,
admits native scalar floor/ceil/sqrt and immutable string length, and improves
failure-only rooted-length diagnostics. It adds no runtime locks, allocation
protocol, executable-memory mechanism or emitter changes. ARM64 and its JIT
remain opt-in; this is not full x86_64 parity.

## Validation

- The complete `arm64_jit_fail_closed_gate.sh` passes, including four-profile
  leaf and string-length contracts, rooted reads/stores/GC/cells/upvalues,
  callback recording, allocations, first-side publication, loop entry, native
  flush/reuse and all four combined M7 FFI profiles.
- The independent caller-result counterfactuals each fail at the intended
  saved-result substitution after disabling only their respective new helper.
  See [caller-result evidence](arm64-jit-lua-caller-result-2026-10-28.md).
- Ordinary ARM64 assertions pass twelve Lua companion contracts and both
  509-case stock source and imported-bytecode corpora. The signed x64 assertion
  build passes both corpora and the return, compound, math and string-length
  companions. Ordinary executables have valid ad-hoc signatures and ARM64 has
  no undefined libatomic helpers or the checked test-hook symbols.
- The main checkout is independently rebuilt as ordinary ARM64 assertions.
  It passes the same twelve Lua companions, both corpora and the inventory.
  Its executable is byte-identical to the restored worktree executable:
  SHA-256 `0ffa77d55e0e67fccb1111c184965d6ccf79c0a04f2c27abe4e9d246f60f4a29`.
- The twenty-workload inventory passes all 260 interpreter/JIT result checks
  on each architecture. ARM64 has selected-root exit witnesses in twelve rows
  versus eighteen on x64. On the original eighteen rows, ARM64 improves from
  seven to eleven; the added dynamic string-length row has a witness too.
  Native-root activity does not prove complete body compilation or performance.

Evidence is retained under
`/Users/frityet/Scratch/ljmt-leaf-primitives-20261028/`: `experimental-final.log`,
`ordinary-matrix.log`, architecture-specific stock and inventory logs,
`main-integration.log`, and the main build/companion/stock/inventory logs.
The original experimental run and intermediate focused failures remain there;
only `experimental-final.log` describes the complete final-source run.

The bootstrap-only gate and separate historical table/FFI matrices were not
rerun for this checkpoint. The experimental gate includes its own combined
FFI matrix and runtime checks. The earlier intermittent rooted-length witness
and callback timeout remain unexplained; the passing run does not fix or
explain them.

## Remaining work

The sampled ARM64 gaps still include branching and nested Lua leaves, varargs,
multiple results and string-byte calls. Neither architecture supplies a native
root witness for the sampled table iterator or I64 arithmetic, so those rows
remain inconclusive. Broader Lua calls, root/side/stitch topology, heap effects,
FFI representations, sustained concurrency and performance also remain open.
Separate branch, byte-read and additional scalar-math proposals are not part
of this integrated checkpoint.

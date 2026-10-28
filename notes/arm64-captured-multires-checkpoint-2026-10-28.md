# Captured calls and fixed multiple-result checkpoint

The complete validation checkpoint is production source `3e3f3432a`. It
combines negative dynamic string-byte indices, parameter and captured nested
calls, fixed two-result pure Lua leaves, and the corresponding independent
semantic/post-register-allocation checks. No locks are added. Two-result
leaves remain bounded direct calls without upvalues, nested calls or branches;
captured nesting retains its one-active-child-snapshot boundary.

## Completed validation

- `arm64_jit_fail_closed_gate.sh` passed in full, including all four combined
  M7 FFI profiles (ARM64/arm64e, assertion/release), native loop/function/side
  contracts, rooted effects, snapshots, lifecycle, callbacks and unwind.
- Every leaf profile passed 16 native fixed-leaf runs, 100 semantic negatives
  and 108 allocated negatives, including the fixed two-result case. The
  nested, child, branch, normalized-argument and return companions also passed.
- Ordinary ARM64 passed fourteen Lua companions, including captured nested
  calls with concurrent closure replacement and same-root reuse.
- Ordinary ARM64 and signed x86_64 each passed all 509 source and all 509
  imported-stock-bytecode cases. x64 also passed its return, compound, nested,
  math and dynamic-string companions.
- Each architecture passed all 260 workload-inventory comparisons. ARM64 has
  selected-root exit witnesses in 17 of 20 rows; x64 has 18. Fixed multiple
  results add the new ARM64 row. Varargs is the remaining sampled difference;
  iterator and I64 rows have no witness on either target.

The evidence directory is
`/Users/frityet/Scratch/ljmt-captured-multires-aggregate-20261028/`.
`tested-source.txt` binds the source, `experimental.log` records the complete
gate, and `ordinary-matrix.log`, the four stock logs, ordinary companion logs,
and both inventory TSVs record the subsequent ordinary builds. The combined
driver completed with exit status 0. The branch was clean throughout these
runs. Later work in other worktrees is not part of this checkpoint.

## Earlier failed aggregate attempts

The `136685c99` aggregate stopped at strict compilation because an inherited
leaf helper retained an unused parameter. `50787dd97` removed it; that next
aggregate reached the GDB JIT build and stopped at four signed/unsigned frame
depth comparisons. `0af10fec9` normalizes the already-range-checked depth to
`MSize`. Both compilation attempts and their source markers are retained under
`/Users/frityet/Scratch/ljmt-captured-aggregate-20261028/`. They are not passing
gates. The combined run above includes both fixes and the fixed-result slice.

## Remaining work

These results do not establish full x86_64 parity. General Lua call/return
shapes, varargs, function-entry and side/stitch topology, heap and FFI coverage
still have explicit boundaries; the separate gap audit and probes track them.
Main `v2.1` remains at `49523abd`. The earlier JIT-enabled harness table
duplication failure has no reproducer or runtime fix, so a clean run with the
corrected interpreter-only harness is not a resolution of that failure.

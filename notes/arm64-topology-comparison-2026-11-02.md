# Fresh ARM64/x86_64 call and iterator topology comparison

The paired native topology baseline uses source `2387b3ed69dc37db4f0c831715610c360e49c6f1`
on branch `codex/arm64-side-ancestry-integration`. The two clean build
worktrees were created from that audit source, whose only additional commit
changes notes. The C/ASM sources are identical to 2387. Later integrated
source is outside the scope of this matrix.

## Builds and run method

Built on macOS with Xcode 27.2 / macOS SDK 27.2 and deployment target 13.0:

```sh
env MACOSX_DEPLOYMENT_TARGET=13.0 make -C src clean TARGET_SYS=Darwin \
  CC=clang TARGET_FLAGS='-arch arm64' \
  XCFLAGS='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL'
env MACOSX_DEPLOYMENT_TARGET=13.0 make -C src -j4 TARGET_SYS=Darwin \
  CC=clang TARGET_FLAGS='-arch arm64' \
  XCFLAGS='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL'
env MACOSX_DEPLOYMENT_TARGET=13.0 make -C src clean TARGET_SYS=Darwin \
  CC=clang TARGET_FLAGS='-arch x86_64' XCFLAGS=''
env MACOSX_DEPLOYMENT_TARGET=13.0 make -C src -j4 TARGET_SYS=Darwin \
  CC=clang TARGET_FLAGS='-arch x86_64' XCFLAGS=''
codesign --force --sign - /path/to/x64/src/luajit
```

The ARM64 executable reports `jit.arch == "arm64"`. The x86_64 executable is
ad-hoc code-signed, passes `codesign --verify`, runs under Rosetta with
`arch -x86_64`, and reports `jit.arch == "x64"`. Both report LuaJIT
`2.1.1793623470`. Executable SHA-256 values:

| Target | SHA-256 |
| --- | --- |
| ARM64 | `fe257ff4b8c46d0cb44da1d66f050e5c373bf856a1dfceb0c0f9fa5fe0d0298d` |
| Signed x86_64 | `2e040da0f5d557fc5c81aa137a78115417fd22bd1216d02f346091b1de83e5e9` |

`bench/jit-topology-probe.lua` recursively disables the complete oracle
prototype tree and keeps all harness helpers and the MT peer interpreted. It
compares candidate/oracle success, exact error text, result count and each
result for 80 calls plus one post-GC call in each phase. Exit callbacks are
classified immediately against the selected runner's currently patched root
bytecode and the live trace's `linktype`/parent. A published trace is never
counted as execution; selected root exits and root-side link targets are
separate fields; side exit IDs are paired with the selected root ID when the
trace metadata reports that root link. This does not claim every side trace
was directly parented by the root.

The corrected topology logs are in
`/Users/frityet/Scratch/arm64-topology-audit-20261102/arm64-topology-forl-chain-baseline.tsv`
and `x64-topology-forl-chain.tsv`. The original broad parity logs remain
in the same directory as `{arm64,x64}-parity-final.tsv`.

The FUNCF explicit-argument follow-up used the frozen signed x86_64 executable
and a rebuilt ARM64 executable with the current experimental source. Both
processed 162 candidate/oracle calls; x86_64 selected root 3 and observed five
root exits on bad input, while ARM64 published no root and observed 81 helper
exits. A later failure-only inspection of the ARM64 FORL root snapshot found
one restoring child-frame row. These follow-up runs do not represent a matched
source build pair. Logs:

- `/Users/frityet/Scratch/arm64-topology-audit-20261102/x64-topology-funcf-explicit.tsv`
- `/Users/frityet/Scratch/arm64-topology-audit-20261102/arm64-topology-funcf-explicit.tsv`
- `/Users/frityet/Scratch/arm64-topology-audit-20261102/arm64-topology-chain3-after-upvalue.tsv`

## Results

| Shape | ARM64 | Signed x86_64 | Result/evidence |
| --- | --- | --- | --- |
| FORL-rooted captured helper chain with non-tail continuation | No selected root in the valid phase; the bad-input phase produces 81 helper-trace exits | Root loop trace 2 is selected and produces 11 root exits plus 1,439 exits from root-linked side traces in the valid phase | The root loop calls `top`, then `top` calls `middle`, and `middle` calls `bottom`; each helper captures only its immediate child. The fixture passes 162 candidate/oracle comparisons on each target. The runtime call chain is deep, while the rejected ARM64 root snapshot contains only one restoring `SNAP_FRAME` row; the full chain crosses trace ancestry. |
| FUNCF root with explicit top/middle/bottom helper arguments and non-tail continuation | No selected root; 81 helper-trace exits on bad input | FUNCF root 3 is selected and produces 5 root exits on bad input | This separate case isolates the general FUNCF caller grammar. Both targets pass 162 candidate/oracle comparisons. |
| Captured, stateful iterator closure returning two values | No selected loop root or exit witness in four phases | Selected loop root 1 exits 5 times in phase 1; 5 later helper exits are classified separately | All 324 candidate/oracle calls agree. ARM64 lacks a selected-root witness for this captured/stateful iterator shape. |
| Recursive Fibonacci | No selected root or side traces | `up-recursion` root 1 and root-linked sides; exit events identify side IDs linked to root 1 | All 243 candidate/oracle calls agree. Recursive trace topology differs. |
| `pcall` around a pure leaf | No selected trace | No selected trace | Semantics agree, but this sample is inconclusive about compiled protected-call parity. |
| Fixed vararg forwarding into open `CALLM` | Selected ARM64 loop root exits; x86 has root and many linked-side exits | Selected x86 loop root exits; side IDs are separately identified | Both run native root code; this bounded forwarded-vararg shape is covered on both. |
| Fixed open-result producer consumed by `CALLM` | Selected ARM64 loop root exits | Selected x86 loop root exits | Both run native root code for this bounded open-result shape. |
| Canonical custom iterator and nil-terminated iterator | Selected ARM64 iterator-loop root exits on exercised phases | Selected x86 iterator-loop root exits on exercised phases | These do not establish arbitrary iterator closure parity. |

The broad `bench/jit-parity.lua` matrix passes 260 interpreter-equivalence
checks on each fresh executable. Both targets have observed selected-root
exits in 18 of 20 workloads; the iterator and I64 arithmetic rows have no
selected-root exit witness. This aggregate sample hides the call-depth and
stateful-iterator gaps above, which is why the topology probes remain useful.

## Scope and next work

This is diagnostic coverage, not a parity completion claim. The fresh
comparison shows two separate call-topology gaps: the captured FORL chain
crosses trace ancestry while the root snapshot records one restoring child
frame, and the explicit-argument FUNCF caller grammar is not admitted on
ARM64. The next step is to bind each root-linked side and nested child return
to its exact caller/continuation, and to broaden the FUNCF caller source proof
without weakening those existing constraints. The captured/stateful iterator case separately indicates that a
future iterator expansion must account for captured mutable state and
multiple results. Protected-call compilation remains inconclusive in this
matrix and needs a workload with an exact native witness on at least one
target before its gap can be described.

Existing iterator, vararg-forward, multires, recursion and protected-call rows
were reused instead of creating duplicate cases. The captured FORL chain,
explicit-argument FUNCF root, and captured/stateful multi-result iterator were
added as separate missing cases.

## Exact source rejection path on the three-child chain

A bounded failure-only ARM64 debug build was made from audit branch
`codex/arm64-parity-audit-20261102` at `c52387e0e` (whose C/ASM base is
`2387b3ed6`). It ran `bench/jit-topology-probe.lua` with the recursive
interpreter oracle. The fixture's 162 candidate/oracle comparisons continued
to agree; it still published no selected root.

The rejected chain reaches the following conservative checks in order:

1. `arm64_lua_leaf_ir_eval_multi()` replays the nested source and reaches a
   `BC_UGET` in the deepest helper. The rooted UGET IR call and matching
   snapshot PC exist, but `arm64_upvalue_ir_context()` rejects the ordinary
   two-row call path because its two-row rule requires both frame rows to be
   the same zero-formal vararg function. This is the first concrete gap; it is
   not a missing UGET opcode implementation or a missing IR helper.
2. After a temporary experiment selected the deepest ordinary frame only
   after validating every CALL edge, replay advanced to a third nested fixed
   `BC_CALL` and hit the evaluator's `depth >= LJ_ARM64_LUA_MAX_CHILD_FRAMES`
   bound. This bound is separate from the active-frame parser's capacity.
3. After a temporary three-call evaluator bound, semantic child admission
   reached its root-only assumption: `arm64_lua_leaf_ir_child()` compares the
   deepest frame base against `2+2*LJ_FR2+bc_a(call)`, although `call` and its
   return PC are still derived from the root prototype. Selecting the deepest
   row as caller instead is also insufficient: the return-map proof then sees
   descendant-prototype snapshot footers and cannot use its one-caller
   `arm64_lua_leaf_ir_result()` replay.

These experiments did not admit the root and their source edits were discarded.
They demonstrate that raising the evaluator depth or changing the frame cap
alone cannot establish safety. The sound route is to retain the exact bounded
frame-path ledger, classify ordinary CALL ancestry separately from
`FRAME_VARG`, bind each row's function/base/link to its own caller CALL, replay
nested pure calls from the outermost child, and bind the deepest active PC and
all returns to those exact ancestors. Each row and each return edge needs
independent semantic and post-RA location/lifetime checks before admission.

Failure-only ARM64 logs and the discarded scratch patch are retained in
`/Users/frityet/Scratch/arm64-parity-audit-20261102-evidence/`.
The baseline paired architecture topology logs above remain the native witness;
these follow-up source diagnostics do not claim a successful ARM64 root or a
matched fresh x86_64 source build.

## Integration of the diagnostic probe

The expanded benchmark and this note are also retained on
`codex/arm64-parity-20261102`, without the unproved recorder/frame-map source
experiments described above. An independent rerun of the committed probe
against both frozen baseline runtimes passed every candidate/oracle check.
Its logs are `arm64-expanded-topology.tsv` and `x64-expanded-topology.tsv` in
`/Users/frityet/Scratch/ljmt-parity-checkpoint-20261102/`.

The rerun again had no ARM64 selected root for either three-child case, while
x64 had a selected FORL root (10 root exits and 6,323 root-linked side exits
in the valid phase) and a selected FUNCF root (five bad-input exits). Trace
IDs and event counts can change across runs; they are observations from the
named logs, not fixed expected counts. A side's root link proves its target
association, not direct parentage.

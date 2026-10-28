# ARM64/x64 trace-topology probe (2026-10-28)

This probe compares three deliberately small trace shapes against an
independently loaded, recursively JIT-disabled interpreter oracle:

- a hot scalar function entry followed by a type-changing call;
- a hot non-leaf function which calls a scalar child and returns through its
  caller;
- a numeric loop whose conditional takes two different hot exits, then
  attempts to heat a second side.

The script keeps an MT peer attached and waiting throughout every workload.
It installs only the `texit` observer, never the `trace` observer (the latter
changes ARM64 side-publication behavior). Every invocation is compared to its
oracle; the bad-type phase compares both success status and exact error text.
Exit rows count native exit callbacks by trace and exit number. A published
trace row by itself is not counted as native execution; in these probes an
observed `texit` from that trace is the native execution witness.

## Build and reproducibility

Both saved executables were built from source commit `3e3f3432a` in the
isolated `codex/arm64-topology-probe` worktree, LuaJIT version
`2.1.1793229499`, macOS deployment target 13.0. The ARM64 binary used
`TARGET_SYS=Darwin`, `CC=clang`, `TARGET_FLAGS='-arch arm64'`, and
`XCFLAGS='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL'`.
The x86-64 binary used the same target/compiler/deployment settings with
`TARGET_FLAGS='-arch x86_64'` and empty `XCFLAGS`; execution reported
`jit.arch == 'x64'` under Rosetta. These are saved immutable binaries, not
executables rebuilt from a later checkout:

| target | SHA-256 | module bundle |
| --- | --- | --- |
| ARM64 | `e0b05b88d04c170cbd32e2cf78c357b35af754b8b10b9566c78dc8a7236251f8` | `/Users/frityet/Scratch/ljmt-topology-probe-20261028/arm64` |
| x86-64 | `1b54fa6530fb02693b757d514257915e373bdd68ebf05153c9e1da3cded1a994` | `/Users/frityet/Scratch/ljmt-topology-probe-20261028/x64` |

The module bundles have matching hashes for `threading.lua` and all common
loaded `jit` modules (including `jit/vmdef.lua`). Run from this source tree:

```sh
env LUA_PATH='/Users/frityet/Scratch/ljmt-topology-probe-20261028/arm64/?.lua;/Users/frityet/Scratch/ljmt-topology-probe-20261028/arm64/?/init.lua;;' \
  /Users/frityet/Scratch/ljmt-topology-probe-20261028/arm64/luajit bench/jit-topology-probe.lua
env LUA_PATH='/Users/frityet/Scratch/ljmt-topology-probe-20261028/x64/?.lua;/Users/frityet/Scratch/ljmt-topology-probe-20261028/x64/?/init.lua;;' \
  /Users/frityet/Scratch/ljmt-topology-probe-20261028/x64/luajit bench/jit-topology-probe.lua
```

Full outputs are preserved at
`/Users/frityet/Scratch/ljmt-topology-probe-20261028/arm64-probe.log` and
`/Users/frityet/Scratch/ljmt-topology-probe-20261028/x64-probe.log`.

## Results

| shape | ARM64 | x86-64 |
| --- | --- | --- |
| hot scalar function | no trace publication and no exit witness in either phase | root trace 1 (`return`, 5 IR instructions); changing the input type produced five exits from trace 1 and an interpreter trace |
| non-leaf function/return | no trace publication and no exit witness in either phase | root trace 1 (9 IR instructions) and child trace 2 (4 IR instructions); changing the input type produced five exits from each and interpreter traces |
| loop with two hot branch outcomes | root loop trace 1 published; phase 1 exited 81 times at exit 4, later phases 3,807 / 2,995 / 3,798 times at exit 1; no root-linked side trace published | root loop trace 1 published; first alternate outcome published side trace 3 linked to root 1; later outcomes produced 3,807 side exits at exit 1 and 81 at exit 2; no second side trace published |

The results establish a concrete topology difference, not a general parity
verdict. In particular, the function-root rows show **no ARM64 witness**;
without recorder diagnostics this probe does not identify which admission
predicate prevented publication. For the loop, ARM64 publication and native
execution are both proven by the root row and its exits, but there is no side
publication. x86-64 demonstrably publishes and executes one side trace, while a
second side is not observed on either target. The sampled loop therefore does
not support a claim that x86-64 publishes a second side.

## Source context

The ARM64 root path is guarded at `trace_arm64_root_recorder_preflight()` in
`src/lj_trace.c`; its root generation gate accepts loop, selected numeric
`FORL`, or selected function-entry shapes. `lj_trace_arm64_lua_leaf()` is a
separate bounded source recognizer for permitted Lua callees/return frames; its
current grammar is visible in `lj_trace_arm64_lua_leaf_range()` and the main
bytecode walk in the same file. Recorder-side first-side checks are in
`trace_arm64_first_side_recorder_preflight()` and
`lj_trace_arm64_first_side_loop_valid()`. Publication additionally requires
the exact captured parent certificate in `trace_arm64_first_side_publish_enabled()`.
The source gate and emitter admission are intentionally separate: the latter
has distinct semantic and post-allocation checks in
`src/lj_asm_arm64_admit.h`.

This probe does not alter any admission boundary or infer a refusal reason
from missing trace rows. It supplies a reproducible workload for future
targeted diagnosis of generic function-entry and side-topology gaps.

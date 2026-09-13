# Original Report Provenance

The original lockless LuaJIT implementation report is kept locally as the
ignored archive `luajit-mt-report.zip`.

- SHA256: `92ea95dbe7dbc6f39e72a44a0d7518e1bef7a9837cc8fb2fff42c5bfc7072957`
- Git ignore rule: `.gitignore` ignores `luajit-mt-report.zip`.
- Current `plan/` files are the working implementation plan and may contain
  deliberate implementation deltas. Avoid broad rewrites: preserve original
  intent when possible, and when changing plan text, keep the old requirement
  recognizable or note the replacement rationale nearby.
- At the time `plan/` began being tracked, the working plan already differed
  from the original archive in the top-level markdown files due to earlier
  implementation deltas such as x86-64-only scope, no `LJ_MT` build wall, and
  milestone status updates. The archive hash above is the baseline for checking
  original wording when a later edit needs comparison.

Archive entries at the time tracking began:

```text
11_ffi_concurrency.md
05_gc_concurrent.md
06_concurrent_objects.md
12_implementation_plan.md
09_threading_api.md
07_interpreter_vm.md
13_testing_and_benchmarks.md
auxiliary/
auxiliary/nbtab_model.c
auxiliary/tests/
auxiliary/tests/t-ffi.lua
auxiliary/tests/t-str.lua
auxiliary/tests/kitchen.lua
auxiliary/tests/t-bc.lua
auxiliary/tests/t-gc.lua
auxiliary/tests/harness.lua
auxiliary/tests/t-co.lua
auxiliary/tests/t-tab.lua
auxiliary/tests/t-api.lua
auxiliary/tests/t-uv.lua
auxiliary/tests/t-litmus.lua
auxiliary/lj_atomic.h
auxiliary/arena_bitmap_model.c
auxiliary/bench/
auxiliary/bench/baseline_jit.txt
auxiliary/bench/run.sh
auxiliary/bench/bench.lua
auxiliary/bench/baseline_interp.txt
auxiliary/bench/bench_mt.lua
10_bytecode_compat.md
04_allocator.md
08_jit_compiler.md
00_README_AGENT.md
02_memory_model.md
14_risks_alternatives_bibliography.md
03_runtime_state_split.md
01_architecture_overview.md
```

Test assertion helpers
======================

Context
-------

Some Lua suites still hand-rolled checks for generated files, captured output,
and expected command failures. These checks are behavior-oriented, but broad
text-pattern helpers made it too easy to recreate source/dump spelling gates
under a different name.

Fix
---

`suite_utils`/`suite_assert` now keep only artifact-named helpers for:

* any-of text marker checks
* exact/plain text containment checks
* expected command failure checks

Generic Lua-pattern and generic source-text assertion helpers were removed from
the active harness. If an invariant needs a constrained implementation shape,
document why beside the code and in a note; tests should cover the observable
behavior or public artifact, not source text, generated IR/ASM text, bytecode
spelling, or generated mcode encoding.

M4 shutdown markers, M5 bytecode dump/load behavior checks, and M9 benchmark
regression failure checks use concrete artifact assertions.

Validation
----------

* `tools/ci/lua_test.sh m4_threading_shutdown m5_cell_ops m9_bench_regression`

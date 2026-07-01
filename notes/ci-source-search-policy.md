# CI source-search policy

CI must not pass or fail by grepping repository source text for helper names,
function calls, field accesses, or implementation snippets. Those checks are
too brittle: they lock in spelling and local structure, but they do not prove
that the runtime behavior is correct.

This policy supersedes older notes that mention deliberate static source guards,
per-case guard scripts, or `suite_utils.read_source_file()`. Treat those older
entries as historical context for why a helper exists, not as instructions to
recreate a source-search test.

The old source-file guard compatibility APIs have been removed from the Lua test
harness. Do not reintroduce generic helpers that read repository source and
search for snippets; use behavior fixtures, generated dump assertions, or
documentation instead.

2026-06-29 audit result: active `tools/ci`, `tools/test.lua`, `tests/suites`,
`tests/lib`, top-level `tests/*.lua`, and top-level `tests/*.c` have no
source-search-only gates left. The remaining string searches inspect generated
JIT dumps, bytecode listings, generated mcode/ASM dumps, captured process
output, CSVs, or marker files. Generic file/text helpers remain available for
those artifacts, but must not be paired with `src/*.c`, `src/*.h`,
`src/*.dasc`, `tests/*.lua`, or `tools/*.sh` snippet checks.

2026-07-01 follow-up: removed the stale M7 Perl guards that read
`src/lj_ccall.c`, `src/lj_cconv.c`, `src/lj_carith.c`, and `src/lib_bit.c`.
Their useful signal now lives in the existing parser-token behavior fixtures:
`t-ffi-ccall-struct-overflow.c`, `t-ffi-cconv-init-snapshot.c`,
`t-ffi-cdata-conv-snapshot.c`, `t-ffi-carith-check64-snapshot.c`, and
`t-ffi-carith-arg-snapshot.c`.

The only supported shell entrypoint under `tools/ci/` is
`tools/ci/lua_test.sh`. Run focused cases as
`tools/ci/lua_test.sh <case...>`; do not add per-case compatibility wrapper
scripts.

Use one of these instead:

- A Lua behavior test when the invariant is visible through public or internal
  Lua behavior.
- A C fixture when the invariant is about object lifetime, publication order,
  native regions, STOPREQ handling, or multithreaded races.
- A generated dump/output check when the invariant is about emitted IR,
  bytecode, machine code, or another build/runtime artifact.
- A note near the relevant helper or in `notes/` when the invariant is design
  guidance that cannot be observed directly.

The only allowed search-style CI checks are over generated artifacts, including
JIT dumps, bytecode listings, objdump output, generated mcode dumps, or
generated assembly. Searching `src/*.c`, `src/*.h`, `src/*.dasc`,
`tests/*.lua`, or `tools/*.sh` for a call name or snippet is not allowed as a
test. Repository DynASM source is source code; generated ASM/mcode output is
the allowed exception.

Examples of invariants that should be documented or tested behaviorally:

- Callback runtime carrier, depth, and auto-detach fields need acquire/release
  helper access because callbacks can hand state between VM threads.
- CTState access needs explicit active-`lua_State` ownership; behavior coverage
  should exercise FFI parsing, cdata allocation, ctype publication, callbacks,
  and snapshot restore paths.
- C-closure upvalue readers need snapshot semantics; behavior coverage should
  mutate debug-visible upvalues and verify interpreter/JIT results.
- Table and local-cell store publication needs runtime and JIT-dump coverage,
  not source checks for specific helper call names.
- Native entropy and mcode paths need behavioral fixtures for STOPREQ/native
  boundary handling rather than source-order assertions.

When replacing an old source guard, keep the useful part: document the memory
ordering, ownership, or publication rule near the helper or in `notes/`, and
add a behavior fixture when a bad implementation would be observable. Do not
encode the implementation spelling as the test.

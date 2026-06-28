# CI source-search policy

CI must not pass or fail by grepping repository source text for helper names,
function calls, field accesses, or implementation snippets. Those checks are
too brittle: they lock in spelling and local structure, but they do not prove
that the runtime behavior is correct.

This policy supersedes older notes that mention deliberate static source guards
or `suite_utils.read_source_file()`.

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
JIT dumps, bytecode listings, objdump output, or generated assembly. Searching
`src/*.c`, `src/*.h`, `src/*.dasc`, `tests/*.lua`, or `tools/*.sh` for a call
name or snippet is not allowed as a test.

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

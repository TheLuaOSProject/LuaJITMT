# Invariant Tests And Documentation

The Lua test harness and CI do not carry source guards. Tests should prove
observable VM behavior, generated compiler/artifact output, or release
packaging. Implementation-only rules should be documented beside the code they
constrain. Matching helper names, function calls, field accesses, or snippets
freezes spelling rather than the concurrency property we care about.

`Test:read()` and `suite_utils.read_file()` are plain artifact readers. They
exist so tests can read generated dumps, captured logs, temporary files,
imported-suite inputs, CSVs, and other runtime artifacts. Do not use them to
predicate a test on repository source text. The harness intentionally has no
source-tree enumeration helper.

Use one of these forms for new coverage:

- A Lua behavior test when the invariant is visible through public or internal
  Lua behavior.
- A C fixture when the invariant is about object lifetime, publication order,
  native regions, STOPREQ handling, or multithreaded races.
- A generated dump/output check when the invariant is about emitted IR,
  bytecode, machine code, or another generated build/runtime artifact.
- A comment beside the constrained helper, plus a note in `notes/`, when the
  invariant is design guidance that cannot be observed directly. The comment
  should explain the ownership, ordering, or nonblocking reason; it should not
  point to a source guard.

String matching remains useful for generated artifacts: JIT dumps, bytecode
listings, objdump output, generated mcode dumps, generated assembly, captured
process output, and generated CSVs. Repository DynASM source is source code;
generated ASM/mcode output is the artifact to inspect.

Historical entries that mention static repository-text assertions, per-case
wrapper scripts, or the old explicit source-reading helper document why a
helper or fixture was added. They are historical context only. The useful rule
from those entries is the invariant itself: memory ordering, ownership,
publication, native-state discipline, or ABI shape.

Examples:

- Callback runtime carrier, depth, and auto-detach fields use acquire/release
  helper access because callbacks can hand state between VM threads.
- CTState access needs explicit active-`lua_State` ownership; behavior coverage
  exercises FFI parsing, cdata allocation, ctype publication, callbacks, and
  snapshot restore paths.
- C-closure upvalue readers need snapshot semantics; behavior coverage mutates
  debug-visible upvalues and verifies interpreter/JIT results.
- Table and local-cell store publication needs runtime and JIT-dump coverage.
- Native entropy and mcode paths need behavioral fixtures for STOPREQ/native
  boundary handling.

Raw-byte/codegen work follows the same split. VM DynASM source should use
mnemonic DynASM syntax for active instructions. Low-level x86 JIT emitter
helpers may centralize opcode constants such as lock-prefixed CAS, but tests
validate the generated mcode and runtime behavior.

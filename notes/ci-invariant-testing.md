# Invariant Tests And Documentation

The Lua test harness and CI do not predicate pass/fail on repository source
text. This is a blanket rule, including old milestone wrapper suites,
one-off historical wrappers, and "this helper name must exist" checks: do not
port them forward. Tests should prove observable VM behavior, generated
compiler/artifact output, benchmark data, or release packaging.
Implementation-only rules should be documented beside the code they constrain.
Matching helper names, function calls, field accesses, or snippets freezes
spelling rather than the concurrency property we care about.

`Test:read()` and `suite_utils.read_file()` are plain artifact readers. They
exist so tests can read generated dumps, captured logs, temporary files,
imported-suite inputs, CSVs, package manifests, and other test artifacts. Do
not use them to predicate a test on repository source text. The harness
intentionally has no repository-source enumeration helper.

Use one of these forms for new coverage:

- A Lua behavior test when the invariant is visible through public or internal
  Lua behavior.
- A C fixture when the invariant is about object lifetime, publication order,
  native regions, STOPREQ handling, or multithreaded races.
- A generated dump/output check when the invariant is about emitted IR,
  bytecode, machine code, or another generated build/runtime artifact.
- A comment beside the constrained helper, plus a note in `notes/`, when the
  invariant is design guidance that cannot be observed directly. The comment
  should explain the ownership, ordering, or nonblocking reason.

String matching remains useful for generated artifacts: JIT dumps, bytecode
listings, objdump output, generated mcode dumps, generated assembly, captured
process output, and generated CSVs. Repository DynASM source is source code;
generated ASM/mcode output is the artifact to inspect.

Historical entries that mention static helper-name wrappers, per-case wrapper
scripts, marker lists, raw-access spelling lists, exact-helper requirements, or
the old explicit source-reading helper are historical context only. They are
not tests to preserve, port, restore, or emulate. When one still matters, carry
forward the invariant itself: memory ordering, ownership, publication,
native-state discipline, or ABI shape. The durable record belongs in comments
beside the constrained code and in notes that explain why the rule exists; CI
must prove it through behavior, C fixtures, generated artifacts, benchmark
data, or packaging outputs.

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

2026-07-03 audit: the active `tools/ci` layer is only the Lua launcher and
platform build smoke script, and the Lua suites do not open repository source
files to decide pass/fail. Remaining file reads are generated dumps, captured
logs, temporary outputs, benchmark CSVs, or release/build artifacts. C fixtures
may still compile against internal headers because they execute lifetime,
publication, native-state, and race behavior rather than grepping source text.

2026-07-03 follow-up: this policy has no historical-suite exception. Old notes
may still describe deleted shell wrappers that once "required" helper spelling
or "rejected" raw field access. Treat that wording as an audit trail for why
the helper/comment exists, not as an active or desired check. When touching
those notes, rewrite the coverage section to name the current behavioral,
fixture, generated-artifact, benchmark, or packaging case that owns the
observable part of the invariant.

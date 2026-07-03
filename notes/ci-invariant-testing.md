# Invariant Tests And Documentation

The Lua test harness and CI must prove observable VM behavior, public API/ABI
compatibility, benchmark data, or release packaging. This is a blanket rule,
including old milestone wrapper suites and one-off historical wrappers.
Repository implementation-text and generated compiler-internal text checks are
not allowed: no source matching, helper-name inventories, raw-field spelling
checks, implementation-shape grep rules, generated IR marker checks, generated
ASM checks, or mcode byte/pattern checks as pass/fail criteria. Implementation-
only rules must be documented beside the code they constrain and, when the
context is broader than a local comment, in `notes/`. Tests should fail on
broken behavior or broken artifacts, not on implementation spelling.

`Test:read()` and `suite_utils.read_file()` are plain artifact readers. They
exist so tests can read captured logs, temporary files, imported-suite inputs,
CSVs, package manifests, release artifacts, opaque bytecode round-trip artifacts,
and other public test artifacts. The harness intentionally has no repository source
enumeration helper.

Use one of these forms for new coverage:

- A Lua behavior test when the invariant is visible through public or internal
  Lua behavior.
- A C fixture when the invariant is about object lifetime, publication order,
  native regions, STOPREQ handling, or multithreaded races.
- A generated artifact check when the artifact itself is the product or
  compatibility surface under test, such as a release archive, install manifest,
  benchmark CSV, captured process output, or a LuaJIT bytecode blob that is
  loaded and executed as an opaque artifact.
- A comment beside the constrained helper, plus a note in `notes/`, when the
  invariant is design guidance that cannot be observed directly. The comment
  should explain the ownership, ordering, or nonblocking reason.

String matching remains useful for public artifacts and process output:
captured stdout/stderr, generated CSVs, package manifests, and release
metadata. Bytecode compatibility tests should load and execute dumps rather
than compare byte spelling. Generated JIT IR, generated ASM, objdump output,
and mcode bytes are compiler-internal implementation spelling and are not active
test gates.

The active assertion helpers intentionally avoid generic regex/pattern
assertions and expose artifact-named containment/count checks only. That keeps
captured output, release metadata, benchmark CSVs, and other public artifacts
easy to check without advertising a reusable source-text gate.

Historical entries that mention old wrapper scripts or implementation-detail
requirements are audit history only. They are not tests to preserve, port,
restore, or emulate. When one still matters, carry forward the invariant
itself: memory ordering, ownership, publication, native-state discipline, or
ABI shape. The durable record is a code-adjacent comment explaining why the
implementation must keep that property, plus a note when the rationale spans
multiple files. CI must prove the observable part through behavior, C fixtures,
public artifacts, benchmark data, or packaging outputs.

Examples:

- Callback runtime carrier, depth, and auto-detach fields use acquire/release
  helper access because callbacks can hand state between VM threads.
- CTState access needs explicit active-`lua_State` ownership; behavior coverage
  exercises FFI parsing, cdata allocation, ctype publication, callbacks, and
  snapshot restore paths.
- C-closure upvalue readers need snapshot semantics; behavior coverage mutates
  debug-visible upvalues and verifies interpreter/JIT results.
- Table and local-cell store publication needs runtime and traceability
  coverage.
- Native entropy and mcode paths need behavioral fixtures for STOPREQ/native
  boundary handling.

Opcode/codegen work follows the same split. VM DynASM source should use
mnemonic DynASM syntax for active instructions. Low-level x86 JIT emitter
helpers may centralize opcode constants such as lock-prefixed CAS, but tests
validate runtime behavior rather than machine-code byte spelling.

2026-07-03 audit: the active `tools/ci` layer is only the Lua launcher and
platform build smoke script, and Lua suite file reads are captured logs,
temporary outputs, benchmark CSVs, opaque bytecode round-trip artifacts, or
release/build artifacts. C fixtures may still compile against internal headers
because they execute lifetime, publication, native-state, and race behavior.

2026-07-03 follow-up: this policy has no historical-suite exception. When
touching old notes, rewrite the coverage section to name the current
behavioral, fixture, public-artifact, benchmark, or packaging case that owns the
observable part of the invariant.

2026-07-03 removal follow-up: active tests, CI, and release workflows keep no
repository text-matching suite. Do not keep source inspections as dormant
scripts, local-only gates, release-only gates, or historical wrapper cases. If
a future cleanup discovers an old repository text-matching gate, remove it and
replace it with a comment/note that explains the reason for the constrained
code plus behavioral or generated-artifact coverage where the failure can be
observed.

2026-07-03 historical-suite follow-up: the old repository-text checks are gone
as tests, including old wrapper names, old source inventories, old bytecode
golden comparisons, and old generated IR/ASM/mcode checks. Do not port them
forward, keep them as dormant tests, or recreate them under a new runner. If one
described a real invariant, document the invariant beside the code that depends
on it and cover the externally visible failure mode through the current Lua/C
fixture, stock-suite, benchmark, or release-artifact harness.

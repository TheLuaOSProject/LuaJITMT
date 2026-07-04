# Lockless Documentation And Observable Coverage

Lockless invariants need an owner that a future maintainer can find quickly.
Use the narrowest durable home for the reason:

- Put local ordering, ownership, native-state, and ABI requirements in comments
  beside the helper, field accessor, VM sequence, or backend emission that
  depends on them.
- Add a note when the reason spans more than one subsystem or when the tradeoff
  needs more context than a code comment can carry.
- Add Lua behavior tests, C race/lifetime fixtures, stock-suite coverage,
  runtime counters, benchmark comparisons, or release/package artifact checks
  when the failure has an observable surface.

Do not keep repository-source text checks as coverage. A test must not read
`src/`, `dynasm/`, or other repository implementation files just to assert that
a helper name, field access, instruction spelling, or forbidden text is present
or absent. Those checks make safe rewrites harder and do not prove the runtime
contract. Put the "why" next to the constrained code, add a note when the
invariant spans subsystems, and use observable behavior coverage when the
invariant can fail at runtime. Keep release, CI, and local harness gates on
behavioral results or product artifacts.

Historical implementation-text cleanup notes are not current design artifacts.
When cleanup history is needed, use git history; current notes should describe
the live invariant and the coverage that exercises it.

Text checks are appropriate only for public artifacts whose text is the product:
captured process output, benchmark CSVs, install manifests, release metadata,
and opaque bytecode dumps that are loaded and executed. Internal implementation
layout is documented through comments and notes, then protected by observable
tests where observable behavior exists.

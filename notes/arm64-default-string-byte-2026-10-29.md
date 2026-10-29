# Default-index string.byte calls

The ordinary ARM64 probe recorded `string.byte(s,1)` and `string.byte(s,nil)`
but refused the equivalent default-index `string.byte(s)`. The preserved
x64 control records all three. The sole source preflight restriction was the
exact two-argument CALL; the existing recorder and independent byte-load
proofs already support a constant zero byte offset.

Preflight now permits one or two arguments while retaining one requested
result, the exact fast-function identity, root source return PC, frame
geometry and actual recorder argument count. Dynamic-index source matching
still requires its original two-argument form. The STRREF/XLOAD, string
ownership, bounds, snapshot and allocator proofs are unchanged. No new
runtime helper or shared-state access is introduced.

The C fixture proves actual selected native execution for the default form,
compares ordinary and NUL/high-byte inputs to a separate JIT-off oracle,
checks unchanged code across full GC, and observes the selected root's
empty-string guard exit before returning to a successful call. Private
semantic and allocated copies independently reject an offset beyond the
proven length and a missing length guard. Independent read-only review found
no actionable issue with the source widening or fixture.

Logs are in `/Users/frityet/Scratch/ljmt-deep-callm-integration-20261029`:

- `default-string-byte-probe.lua`, `default-byte-arm64-before.log`,
  `default-byte-x64-before.log`: matching source probes. ARM64 is the ordinary
  integer-literal checkpoint; the preserved x64 control is `bc0313318`.
- `default-byte-four-profile.log`: complete string-byte contract passes
  assertion/release ARM64 and arm64e, retaining the earlier constant,
  dynamic, negative-index, folded-byte, ownership and corruption cases.
- `default-byte-arm64-final.log`: ordinary no-helper ARM64 observes eight
  selected root exits for eight calls before and after GC for all three
  call forms, with matching interpreter results.
- `default-byte-stock-{source,bytecode}.log`: both ordinary ARM64 corpora
  pass all 509 cases.

The contract and ordinary checks exit zero. Final ordinary assertion ARM64
profile without test helpers:
`ce29bcbc51e7c4b79f51d0284bb3fcf07de306464c0220c98ed783455819d717`.
This covers the one-byte default index; general multi-byte return ranges
and full x86_64 parity remain separate work.

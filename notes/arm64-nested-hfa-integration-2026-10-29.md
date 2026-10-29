# Nested named HFA argument integration

Production commit `d18fea7f2` is integrated as `955d08797` on top of the
validated `22d54430f` checkpoint. The combined aggregate contract passes
ARM64 and ARM64e, assertions and release. The restored ordinary ARM64 runtime
has assertions but no test-helper macros.

## Matching ordinary-runtime witness

`tests/t-ffi-callxs-nested-hfa.lua` uses named `float lo, hi` members inside a
named nested struct, with a separate interpreted oracle and an attached MT
peer. It checks exact C effect counts, a candidate-owned self-linked JFORL
root containing CALLXS, full-GC reuse with the same code bytes/address/loop,
and an alternate-callee guard exit without changing that root's code.

The frozen ordinary baseline from `c60f376f9` passes on x86_64: trace 1,
one selected-root post-GC exit and 31 alternate-callee exits. Its ARM64
counterpart fails the selected native root assertion after matching all
warmup results and effects. The integrated ordinary ARM64 runtime passes:
trace 1, one selected-root post-GC exit and 31 alternate-callee exits. Both
successful runs join their peers. These checks establish the exact named
member shape on both architectures; the earlier audit's nested array shape
remains outside the admitted scope.

Evidence, including the expected baseline ARM64 failure, full profile log,
frozen integrated runtime/library and hashes, is preserved at
`/Users/frityet/Scratch/ljmt-nested-hfa-integrated-20261029/`.

## Completed validation and promotion

The fixture expansion from `9799a13cf` is integrated as `bcc552b45`.
It adds a native sibling-plus-nested float triple with independent source,
member-offset and allocated-carrier rejection cases, plus classifier checks
for nested double quads, the four-leaf limit, mixed extra members, depth
31/32, and result rejection with both NULL and non-NULL descriptors. Review
also fixed the warmup observer to refresh the selected root table after
compilation and clear old exit counts.

The final combined four-profile aggregate contract passes, with the ordinary
named-pair probe now included in every profile. See `aggregate-final-four.log`.
The ordinary no-helper named-pair run, 509 stock source tests, and interpreter
aggregate/variadic ABI contract passed on the same production implementation
before that test-only fixture expansion. No production code changed in the
later fixture, contract-wiring, or documentation commits. The biased-IR
copy crash investigation is closed separately in
`test-helper-biased-ir-copy-crash-2026-10-29.md`.

This bounded slice is ready for the main parity branch. Nested arrays, open
Lua result forwarding and deeper side ancestry remain active work; overall
x86_64 parity is incomplete.

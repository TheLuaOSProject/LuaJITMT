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

Representative broader nested classifier fixtures are being added before
this slice is promoted to the main parity branch. Overall x86_64 parity is
still incomplete.

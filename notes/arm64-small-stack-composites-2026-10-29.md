# Small non-HFA stack composites

This checkpoint adds one-, two-, and four-byte integer-only structs passed
after the GPR argument bank is exhausted. Darwin passes these composites in
eight-byte stack slots while still packing neighboring scalar arguments at
their natural widths. An AppleClang 21 caller independently confirms the
layout. The native witness includes a one-byte scalar prefix, the composite,
and a four-byte scalar suffix: offsets 0, 8 and 16, with a 20-byte outgoing
argument extent.

The probe exposed an interpreter bug as well as the former JIT refusal.
`ccall_set_args` aligned a small struct to eight bytes but advanced only its
payload size. The following scalar therefore landed too early. The frozen
baseline ARM64 runtime returned 211926428 where the byte-struct call's
independent C-expression oracle requires 1153. The exact erroneous value is
incidental stack content; the misplaced suffix is the defect. x86-64 passed
the same explicit oracle and published native loop roots for all three sizes.

The interpreter now advances complete eight-byte slots for non-HFA structs.
The fixed CALLXS emitter consumes the existing classified stack offsets, so
payload width and reserved slot extent are independent. Only the actual
one/two/four-byte payload is loaded and stored; padding is not read from beyond
the CData payload. Semantic and post-RA admission each reconstruct the
eight-byte composite alignment/extent and preserve exact carrier-width checks.
Signature reconstruction remains mandatory in both assembler stages. Existing
variable-arity lowering is unchanged, and the bounded JIT classifier still
refuses variable-arity aggregate signatures.

Validation:

- The complete integer/HFA aggregate contract passed ARM64 and ARM64e,
  assertion and release profiles. This includes the previous ABI-plan fault
  injection and independent source, CType, carrier, snapshot and allocation
  mutations for the new stack cases.
- Ordinary ARM64 execution with a live waiting MT peer matched explicit
  results for byte/half/word structs and observed native selected-root exits
  after full GC (32, 32 and 832 respectively). The word case also exits on its
  accumulator's integer-range boundary; the counts do not imply every loop
  iteration stayed native.
- The same ordinary x86-64/Rosetta probe observed 32 native selected-root exits
  for each size after full GC and matched all oracle values.
- AppleClang strict compilation passed for assembler, recorder and C-call
  units on ARM64, ARM64e and x86-64. An earlier manual compile used the wrong
  compiler/SDK combination and failed before this verified run.

Evidence is in `/Users/frityet/Scratch/ljmt-small-stack-abi-20261029/`:
`caller-apple-arm64.s`, `compiler.txt`, `baseline-direct-oracle.log`,
`final-ordinary-arm64.log`, `dynamic-prefix-x64.log`, and `four-profiles.log`.
The baseline runtimes are preserved in the preceding stack/tail ordinary
matrix. The first local incremental build did not rebuild the recorder after
the classifier header changed; all successful profile tests used clean builds.
The follow-up adds the classifier header to the checked-in recorder and
assembler prerequisites. A dependency-only dry run with that header treated
as changed previously left the recorder up to date; afterward it schedules
both affected translation units. This is build plumbing only.

A later focused candidate probe at this checkpoint also publishes a root when
the byte prefix is a literal. The regression/evidence is recorded in
`notes/arm64-callxs-literal-narrow-stack-regression-2026-10-29.md`. This does
not claim arbitrary aggregate or full architecture parity.

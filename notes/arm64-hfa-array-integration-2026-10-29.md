# Nested HFA array argument integration

Commit `3ed65e667` is integrated as `9bb230fa1` on the named-member checkpoint.
The classifier now flattens fixed arrays through named nested structs and
multidimensional arrays, with at most four homogeneous float/double leaves.
It validates every extent, offset and alignment from CType snapshots. Arrays
set the existing nested-shape marker, keeping all newly added array/nested
result channels independently closed. Anonymous subtypes, references, unions,
variable arrays, vector/complex arrays, padding and over-alignment remain
outside this admission policy.

## Validation

The integrated aggregate contract passes all four ARM64/ARM64e assertion and
release profiles. The final run also executes both ordinary named-member and
array variants of `tests/t-ffi-callxs-nested-hfa.lua` in every profile. The C
fixture independently rejects malformed lane offsets, duplicate/missing
lanes, source bindings and physical carriers, and checks classifier count,
depth and type boundaries. A separate interpreter aggregate/variadic contract
and all 509 stock source tests pass.

The restored ordinary runtime has no test-helper macros. Both it and the
frozen x86_64 ordinary runtime pass the named and array variants with a live
peer: the exact selected CALLXS root's code bytes, address and loop identity
survive full GC, with one observed selected-root exit afterward. The named
variant also observes 31 selected-root exits for the alternate C callee. C
effects match the independent interpreted oracle exactly; peers join.

The original nested-array gap probe also passes on the feature branch and
frozen x86_64 with the same universal DSO and source: root `1:2`, 25 selected
exits and 2,752 effects on each target. This is the exact previously missing
nested float-array argument case, not only an ABI interpreter result.

Integrated logs and immutable artifacts are under
`/Users/frityet/Scratch/ljmt-hfa-arrays-integrated-20261029/`; the original-gap
rerun is under `/Users/frityet/Scratch/ljmt-hfa-array-ordinary-20261029/`.

## Independently resolved publication issue

Repeated unchanged-main topology runs at `8d164b856` exposed intermittent
absence of the captured pair-call or nonleaf FUNCF root: four of five strict
runs failed, while one passed. This also occurs without the experimental
CALLM changes. Evidence is in
`/Users/frityet/Scratch/ljmt-callm-pair-review-20261029/`. The cause was an
unsupported disabled function reaching ARM64 source preflight before the
existing NOJIT entry patch, repeatedly disturbing a colliding hotcount bucket.
Commit `10c064925` fixes that ordering, with a deterministic regression and
five successful ordinary topology runs on each architecture. See
`arm64-nojit-funcf-hotcount-2026-10-29.md` for the evidence. The HFA-specific
native tests above are separate. Overall x86_64 parity remains incomplete.

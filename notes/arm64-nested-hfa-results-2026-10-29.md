# Nested and array HFA results

The ordinary cross-architecture audit found two more exact native gaps:
`struct { struct {float lo,hi;} inner; }` and
`struct { struct {float lane[2];} inner; }` results. At the preceding ARM64
checkpoint both execute through the interpreter, with no selected native
root. Frozen x86_64 publishes a two-CALLXS loop for each, with 25 observed
exits and 2,752 C effects. The source and universal helper are under
`/Users/frityet/Scratch/ljmt-nested-hfa-results-20261029/`.

The ARM64 classifier now uses the same bounded HFA layout for arguments and
results. It no longer rejects a valid result solely because a member is
nested or an array. The existing recorder and assembler already consume the
classifier's exact offsets and return lanes. The existing result-box CType,
payload stores, source snapshots and native-frame protocol checks remain in
place; no assembler or runtime lifecycle change is needed.

All existing flattening restrictions still apply: at most four homogeneous
float or double leaves, tightly packed natural layout, bounded nesting and
wrappers, and no unions, anonymous subtypes, variable/vector/complex arrays,
mixed leaves, padding or over-alignment. Non-HFA small results continue to
use their independent classifier.

## Validation

The aggregate contract passes ARM64/ARM64e assertion and release profiles.
Native result fixtures cover named float pairs, nested array float pairs,
sibling plus nested float triples and nested double quads. They validate
each field against the interpreter, exact C effects, alternate-callee exits,
and full-GC reuse of the same selected machine-code bytes/address/loop while
a peer is live. Each new shape also runs the existing independent semantic
and post-allocation result-box/lane/store/snapshot mutation checks.

The ordinary `tests/t-ffi-callxs-small-composite-results.lua` now includes the
two exact native gaps. Both ordinary ARM64 and frozen x86_64 pass using the
same helper DSO: each retains its selected CALLXS JFORL root across full GC
and an alternate scalar result, with one selected post-GC exit and 31
alternate-callee exits. All fields and C counts match the interpreted oracle,
and the peer joins. The original gap source now also publishes on ARM64 with
the same two CALLXS, 25 selected exits and 2,752 effects as x86_64.

Logs include `baseline-arm64.log`, `baseline-x86_64.log`, `gap-arm64.log`,
`ordinary-{arm64,x86_64}.log`, `focused.log` and `four-profile.log` in the
evidence directory above. A final classifier-only addition exercises NULL
and non-NULL result descriptors for the one-element array, multidimensional
float array and four-double array boundaries; `final-boundaries.log` passes
the assertion ARM64 contract with those added cases. Overall parity remains
incomplete, including the
previously identified non-HFA 24-byte indirect result.

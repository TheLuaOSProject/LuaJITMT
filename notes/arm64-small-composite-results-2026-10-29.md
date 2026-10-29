# Small composite result classification

The shared ARM64 result classifier now accepts fixed 1, 2, 4 and 8 byte
non-HFA structures containing numeric leaves, including named nested
structures and mixed integer/floating members. At least one integer, boolean
or enum leaf must be present, ruling out HFA register channels. These values
use the existing single-GPR opaque payload, result CData box and exact-width
store path; the argument policy and assembler are unchanged.

The walk uses CType snapshots and bounds nesting, typedef/attribute chains,
leaf count, size, alignment and field extents. It rejects cycles, overlapping
fields, unions, arrays, anonymous subtypes, pointers, variable types, empty
structures and over-alignment. Pure floating nested results retain their
separate closed policy. Existing direct integer result classifications use
their original fast path. Both NULL and non-NULL output descriptors are
covered.

## Initial validation

The focused aggregate contract passes ARM64 and ARM64e assertion and release
profiles. Native cases cover `{ {uint16_t lo,hi;} half; uint32_t tag; }` and
`{uint32_t tag; float value;}` results. Each uses a live peer, interpreted
oracle, exact C call counts, an alternate scalar-returning callee, and reuse
of the same selected root machine-code bytes/address/loop across full GC.
Independent semantic and post-allocation mutations reject incorrect result
CType, width, payload store, snapshot root and frame properties. Classifier
tests include a mixed nested result, depth 31 acceptance/depth 32 rejection,
and closed array/pointer member shapes.

The ordinary runtime, without test helper macros, also runs the original
gap-audit source natively for both new result shapes: each selected root has
two CALLXS instructions, 25 observed exits and 2,752 C effects. The larger
16-byte and 24-byte result cases remain closed. A previous x86_64 run
established the two small shapes and the 24-byte indirect result as native;
the latter remains an ARM64 parity gap.

Evidence is in
`/Users/frityet/Scratch/ljmt-small-composite-results-20261029/`, particularly
`four-profile.log` and `results-v2.log`. Early `results.log` and `results-v.*`
preserve a rejected intermediate classifier: it incorrectly tested the
type-dependent CTF_REF bit on unsigned numeric leaves. The corrected walk
uses typed scalar classification and is the version tested above.

## Ordinary runtime and final gate

`tests/t-ffi-callxs-small-composite-results.lua` runs both shapes with no
private test hooks. The ordinary ARM64 runtime at `7b812d6be` and the frozen
x86_64 runtime from the earlier `c60f376f9` matrix both pass using the same
universal C helper library. Each shape retains its exact selected JFORL
CALLXS root through full GC and an alternate scalar-returning callee, with
one selected exit after GC and 31 alternate-callee exits. C effects and all
result fields match the independent interpreted oracle; the live peer joins.

The final aggregate gate includes this ordinary probe in every profile;
`final-four-profile.log` passes all four profiles. The ordinary ARM64 runtime
also passes all 509 stock source tests and the interpreter aggregate/variadic
contract. `immutable.sha256` records the frozen binaries, archive and helper
library. The original result-gap probe was repeated on frozen x86_64 and
again shows the 24-byte indirect-result gap, while neither architecture
publishes a native root for its 16-byte direct-result cases. Overall x86_64
parity remains incomplete.

# Nested and array HFA CALLXS arguments

The bounded Darwin ARM64 HFA argument classifier now recursively accepts named
struct members and fixed-size homogeneous float/double arrays, up to four scalar
leaves. CType traversal uses stable snapshots, tracks active type IDs and a
recursion bound, checks each field and element extent/alignment, and preserves
the outer by-value CType identity. Arrays can nest through structs and
multidimensional arrays. Anonymous `CTA_SUBTYPE` members remain closed.

Arrays set the shape's nested marker. The HFA result classifier therefore
continues to reject nested and array result channels even when called with a
NULL output descriptor. The gate also rejects VLA, vector, complex, reference,
union, over-aligned, padded, mixed-leaf, and more-than-four-leaf shapes.

The native fixture covers both the actual nested-array gap shape
`struct { struct { float lane[2]; } inner; }` and a sibling-plus-nested named
float triple. They compare weighted C results with independently JIT-disabled
oracles while a threading peer remains live. The tests require selected-root
native entry/exit, full-GC re-entry, and exact effect counts. The array trace is
also independently corrupted in semantic and post-RA views by changing each
lane's stride/offset, duplicating a lane, omitting a lane, and changing the
source snapshot. Classifier cases cover one, two, and four float leaves, four
double leaves, a fifth-leaf rejection, depth 31/32, over-alignment, complex and
VLA arrays, anonymous subtype rejection, and nested-result NULL/non-NULL
descriptor rejection.

## Ordinary gap probe

The original saved cross-architecture probe and universal `gap.dylib` were
rerun. The ARM64 run used the ordinary assertion runtime from this branch, with
matching `src` Lua modules and no test-helper symbols. The x86_64 run used the
frozen b1d19 ordinary runtime and its matching module bundle. Both used the
same probe source and universal C library. Their `nested-sse-arg` row reports
`roots=1:2`, `selected-exits=25`, and `effects=2752`; all 11 workloads complete
25 oracle rounds, full-GC reuse, and exact C effect checks.

Evidence, logs, hashes and copied probe source are under
`/Users/frityet/Scratch/ljmt-hfa-array-ordinary-20261029/`. The saved universal
library and original probe came from
`/Users/frityet/Scratch/ljmt-ffi-gap-audit-20261029/`; the frozen x86_64
runtime is under
`/Users/frityet/Scratch/ljmt-hfa-multilane-integrated-20261029/ordinary/x86_64`.
This closes the specific nested float-array argument gap from the earlier
ordinary audit. Other aggregate classes and HFA result channels remain outside
this change.

The focused `arm64-assert` run of
`tools/ci/arm64_jit_integer_aggregate_args_contract.sh` passed with the nested
float triple, array, classifier boundaries, and semantic/post-RA mutations.
The root will run the four-profile matrix before integration.

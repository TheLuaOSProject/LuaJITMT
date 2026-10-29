# Deep side ancestry capacity follow-up

The next topology gap is reproducible at production `bc0313318`. With
`maxtrace=1024`, `maxside=1000`, and matching generated 70-arm while source,
ordinary x86_64 publishes root-linked native bodies through depth 69. The
last body observes three selected exits while warming and two selected exits
after full GC. Ordinary ARM64 stops at 65 bodies (64 children), and the last
admitted side handles repeated guard exits for later branches: 483 cumulative
exits by mode 70 and 255 after GC. Every result matches a separately loaded,
recursively JIT-disabled oracle, with a live peer.

Evidence is in
`/Users/frityet/Scratch/ljmt-deep-callm-integration-20261029/`:
`capacity-probe.lua`, `arm64-capacity70.log`, `x64-capacity70.log`.
The probe counts only root/loop link types when selecting the deepest native
body, so a later interpreter exit trace does not masquerade as a native
ancestor. The preserved x86_64 runtime/modules are in `x64-frozen/`.

An earlier 80-arm run used the default `maxside` and therefore is not the
matched capacity witness: x64 hit its configured side budget before ARM64's
deep ancestry cap. Its ARM64 log still documents repeated exits above depth
64. Use the explicit-maxside 70-arm pair for the architecture comparison.

## Existing bounds

`TRACE_ARM64_SIDE_GRAPH_MAX` limits the stack index to 64 list members, with
an oversized fallback deliberately accepting only depth one or two.
`LJ_ARM64_SIDE_MAX_ANCESTRY` independently limits the copied assembler parent
chain to 64 rows; both semantic and allocated views store the count in a
byte. Raising only one constant cannot close the gap safely. Root and middle
retirement, direct-ancestor lookup, snapshot capture, and copied admission
all depend on these bounds. Native entry must remain nonallocating.

## Implementation direction under review

Retain the current small indexed fast path. For larger graphs, validate the
root-owned prepend-ordered list against the existing trace-vector limit,
requiring every parent later in the same list and every immutable parent
certificate/generation to agree. This gives an acyclic ordering without an
arbitrary depth cap or an allocation inside an SMR reader. The larger-graph
path must revalidate root head/count/vector and child generations before use.
Its cost must be assessed separately from the common <=64 path.

The copied assembler ancestry needs storage sized from the validated root
list, rather than a larger unconditional stack array. A recorder-token-owned
scratch allocation can be reserved before entering SMR, retained across
assembly retries, and freed with existing JIT scratch at VM close. Capture
must refuse if the live validated list exceeds its reserved capacity; it
cannot allocate under SMR. The view must carry a capacity and a count wide
enough for the trace vector. Independent test clones must own their copied
rows instead of mutating recorder scratch. Publication and retirement must
use the same ordering invariant, including partial/middle retirement and
full flush followed by trace-number reuse.

No production capacity change is included yet. Required validation includes
65-to-66 and larger native chains, source/certificate/order/cycle/generation
negatives beyond the old bound, ordinary matched x64 behavior, full GC and
exact code reuse, middle/root teardown and repeated native reentry, plus all
four ARM64 profiles and the existing wide shallow graph cases.

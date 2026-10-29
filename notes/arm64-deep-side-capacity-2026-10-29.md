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

## Copied ancestry storage preparation

The assembler snapshot now retains its inline 64-row storage for ordinary
chains and can select recorder-owned reusable storage for a deeper chain.
Reservation occurs outside the SMR read section and the exact parent view is
revalidated after reacquiring it. The allocation is based on parent depth,
not sibling-list width, and is released with the JIT state. Private-copy
admission tests detach large ancestry rows before mutating them.

The ARM64 assertion side-admission contract passed with this preparation
(`capacity-storage-assert.log` in the integration evidence directory), and the
ordinary profile was restored. The publication capacity policy is unchanged
in this checkpoint; the larger allocation path has not yet been exercised by
a native chain and this is not a capacity-parity claim.

## Native capacity extension

The 64-row graph index remains the small-list path. Larger lists now validate
exact root membership, immutable parent generations and prepend order without
allocating in metadata readers. Non-root parents must occur after each child
in the root-owned list, so cycles and reordered ancestry are rejected. Native
entry, assembler snapshots and descendant-first retirement use the larger
proof; no recorder lock or VM/FFI lock was introduced.

The first ordinary probe (`arm64-capacity70-open.log`) reaches 70 native
bodies, depth 69, with three deepest exits on the three mode-70 calls and eight
after-GC deepest exits on eight further calls. This closes the matched probe's
old ARM64 depth-64 stop; it does not claim equal x64 trace topology or speed.

`capacity-four-profile.log` passes the expanded side contract in all four
ARM64/arm64e assertion/release profiles. It covers depth 15, 64, 69 and 80;
code/generation stability through GC; exact native deepest exits; large-list
leaf and ancestor scoped teardown; full flush/reentry; reuse of reserved
ancestry storage; and existing wide-root scenarios that now grow past the old
64-child refusal boundary. Private-copy negatives include late-chain
generation corruption, insufficient capacity, and capacity above the trace
number representation. The final ordinary restoration profile is
`0a57a2f1b36da2be6f9ac5f9bcd8744bdd5f1d1838072e431db5e11b45dd1be9`.
The previous assertion-only run correctly stopped at a historical fixture
expectation that a valid extension must be refused; that expectation was
updated, not the result/error or malformed-ancestry oracles.

An independent read-only review found no concrete membership or SMR/lifetime
bypass. It identified quadratic suffix scanning on deep large graphs as a
performance concern to reduce before integration. Large-list limits follow
the existing trace-number/count representation; a new arbitrary depth cap is
not introduced. Full integrated gate validation is still pending.

## Large-list membership cost

A follow-up separates complete list/certificate validation from ordered parent
membership. After the first pass proves exact length and termination, the
second stops each suffix search at its parent and revalidates the root tuple
again. Immediate-parent chains therefore have linear membership scanning;
widely separated mixed branches can still require quadratic searches. Native
publication also reuses the captured parent's actual depth instead of
computing every sibling's maximum depth. No locking or shared scratch is used
by these metadata readers. This is an algorithmic bound, not a speed claim.

The complete expanded side matrix passes again in all four profiles
(`capacity-linear-four-profile.log`), with ordinary restoration profile
`74c852e91ca561a40a2f63da680a30c5cbe0dc3e67f8a90dbae6810f544b9cdd`.
A read-only follow-up review found no additional membership/revalidation
blocker. Larger-source probes with 133 and 260 branches reveal a separate
first-side semantic refusal before any deep chain is formed; x64 reaches
depth 259 on the 260-branch source. Those probes do not test the extended
ancestry count yet, and the cause is under investigation.

## Complete integrated gate

The complete gate passes at pinned integration `28c9ec72b`, which contains
the reusable ancestry storage, capacity extension and two-pass membership
validation. The final VM and recorder safepoint runtime tests actually ran;
all four M7 FFI profiles passed and no contract was skipped. The outer
command exited zero and rebuilt ordinary assertion ARM64 without helpers.
Evidence is `capacity-complete-gate.log` and
`capacity-complete-restoration.log` in the integration evidence directory.
The ordinary restoration profile is
`6b413f447b6e68b052094d38a337a397c0a6368c125d09f6899f58dd0b1d6adc`.

This is the new verified capacity checkpoint. It does not include the later
sparse-parent snapshot correction or numeric-cdata variadic FFI changes;
their combined validation remains separate. General Lua call/return,
recursion, protected calls and other source/FFI restrictions still prevent a
full x86_64 parity claim.

The promoted main continuation was independently rebuilt with no test helpers
(`capacity-main-build.log`), ordinary assertion profile
`e6a05f3e324a4bd4a8c908b68473f615ed5477219e9a7be17066303dd2ea583b`.
Its source and original-v2-bytecode stock corpora both pass all 509 cases
(`capacity-main-source.log` and `capacity-main-bytecode.log`).

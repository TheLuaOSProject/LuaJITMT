# Scalar array iteration during IDLE reclamation

Ordinary Lua `next` and ITERN can now finish scalar-array iteration while an
unrelated IDLE metadata reclaimer is paused. After global SMR admission fails,
the iterator independently retains the small table and array allocations,
validates the actual source cells and state owner, and returns only a number,
boolean or fully validated end-of-iteration result. A refused attempt releases
all authority before the existing retry path. The ordinary admitted reader
path is unchanged.

This path requires the permanent nil hash node and valid small-array geometry.
It never follows an opaque GC value, skips a GC edge or substitutes END for
refusal. FOUND and END both revalidate source, owner and vector generation.
Output stores follow all input reads, preserving aliases; each retry restores
stack-backed output addresses after any earlier stack movement. Existing
publication and lifetime rules remain.

The combination with the accepted worker and constructor-fairness runtime
passes 127 isolated processes and 49 registered components. The four isolated
builds distinguish default, optimized GC2 helpers, ten-flag assertion/APICHECK,
and target-only ASan with leak checks. Stock off/on passes 387/509 cases in
each. The 24 paused-writer cases per helper build cover VM next, witnessed
ITERN, direct rooted next and internal cursor across six array shapes.
Authority, opaque/protected inputs, aliasing, real resize/collection, moved
stacks and physically freed vector controls retain their original oracles.
Both exact current-baseline negative controls still hit their original
four-second alarms. The earlier owner package separately has 123 final passes
on its older runtime generation; development failures remain preserved.

The new `m5_tab_scalar_next` case runs 44 components and restores the default
build. The original `m6_jit_alloc_account` now completes all five components,
including the formerly stalled IDLE-entry fixture and its two previously
unrun cooperation cases. No original paused window or timeout was relaxed.
The permanent authority fixture drops its evidence-version suffix; the
dependent lifetime fixture changes only that include name and its comment.

Seven alternating cost pairs per workload retain 56 processes and 280 samples,
plus the separate eight-process pilot. Median paired changes are +0.44% for
interpreted next, +0.17% for interpreted ITERN, +1.59% for JIT-enabled next,
and +0.11% for JIT-enabled ITERN. The JIT-enabled next ranges do not overlap:
39.62 to 40.20 ns per visited value in the medians. That small measured cost is
retained as a tradeoff for the progress repair; it is not dismissed as noise.
The other ranges overlap. These fixed-array, single-mutator measurements do
not establish general application cost, stock parity or contention throughput.

The scope is deliberately narrower than general iterator progress. Public C
`lua_next` still waits in receiver capture before reaching this helper. The
separate capture candidate stopped during automatic review and remains
unlanded; its interrupted validation was not retried here. Hash/GC-valued
iteration, Huge/custom allocations, an independently held plain-arena writer
and broader asynchronous reclamation still require their own lifetime and
progress proofs. Small-lease metadata loops retain their existing limits.

See the [combined review](evidence/tab-scalar-next-2026-09-05/root/review.md),
[original admission proof](evidence/tab-scalar-next-2026-09-05/proof/source-proposal.md),
[owner validation](evidence/tab-scalar-next-2026-09-05/owner/final-review.md),
[registered results](evidence/tab-scalar-next-2026-09-05/root/canonical.json),
[paired costs](evidence/tab-scalar-next-2026-09-05/root/perf/summary.json),
and [archive manifest](evidence/tab-scalar-next-2026-09-05/manifest.json).

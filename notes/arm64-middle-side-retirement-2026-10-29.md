# ARM64 middle-side scoped retirement

The depth-three side-graph fixture now flushes its middle ancestor. The middle
trace and its descendant are retired, while the direct child and canonical
root retain their original code pointers and generations. After a full GC, the
direct path still enters natively, exits through its selected root snapshot,
and preserves the one-child topology.

The retirement planner previously treated a scoped pending ancestor exactly
like an ordinary runnable trace. During scoped marking, however, descendant
edges can already have been moved to the parent's fallback while those
descendants still remain in the root-owned sibling list. The mark-phase plan
now has an explicit `allow_pending_descendants` mode, enabled only by scoped
edge detachment. It accepts a pending endpoint only when its sealed generation
certificate remains valid and the exact edge still names either its child or
the authenticated fallback. Retirement/unlink planning remains strict: a
parent body cannot be retired while descendants remain linked.

The test-only proof fixture exercises both modes on the live depth-three
graph. Strict mode rejects the pending middle generation; scoped mode accepts
it. Independent corruptions of the sealed certificate generation and the
inbound edge target are rejected, and every mutated field is restored before
the real scoped flush. The test asserts all four results.
The canonical root remains runnable throughout this middle-ancestor case; it
does not claim support for scoped teardown of the root itself.

The contract script keeps its default behavior bound to the already-built
profile (including archive/object freshness checks); this is required when the
aggregate gate invokes it inside a profile. A separate opt-in
`LJ_ARM64_SIDE_PROFILES` mode runs named profiles and restores the caller's
profile afterward.

Validation: the opt-in matrix passed `arm64-assert`, `arm64e-assert`,
`arm64-release`, and `arm64e-release`. The final run log is
`/Users/frityet/Scratch/ljmt-deep-ancestor-retirement-20261029/four-profile-negative-matrix.log`.
The contract restored the original ARM64 target and flags on completion.

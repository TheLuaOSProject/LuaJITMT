# Nested homogeneous float aggregate CALLXS arguments

This checkpoint adds bounded Darwin ARM64 HFA arguments made from named
struct members. Recursive CType traversal uses stable snapshots, tracks active
type IDs and a depth bound, verifies exact field offsets and aggregate extents,
and preserves the outer by-value CType identity. The independent semantic and
post-RA passes validate raw XLOAD offsets, source snapshots, CALLXS mapping and
physical FPR carriers in their own walks. A named sibling-plus-nested float
triple now has a live native CALLXS/exit/full-GC-reentry fixture; a named nested
double quad and 31/32-level depth boundary are classifier fixtures. The nested
result classifier is tested with both a NULL and non-NULL output descriptor.

Arrays and anonymous `CTA_SUBTYPE` members remain closed. Nested HFA results
remain closed; the result classifier explicitly rejects nested shapes even
when its caller omits the output descriptor. Unions, references, VLA/vector
members, mixed scalar leaves, padding and over-aligned nested wrappers fail
classification.

The native pair fixture uses `struct { struct { float lo, hi; } inner; }`; the
triple fixture uses a sibling scalar plus a named two-float nested member. Both
check weighted C results against JIT-disabled oracles with a live threading
peer. The pair case observes selected-root exits, full-GC re-entry and an
alternate callee exit. The triple case independently observes native entry and
exit, full-GC re-entry, and semantic/post-RA source and per-member-offset
corruptions. Signature boundaries cover five leaves, a non-float extra member,
31 versus 32 nested levels, nested results, unions, over-aligned nested members
and arrays.

Scope correction: the earlier x86_64 witness that motivated this work uses a
nested float array. Arrays remain explicitly rejected here, so that original
x64-vs-ARM64 parity gap is still open. The new native triple has named members
and is a representative extension, not evidence that the array case is closed.

## Validation

The focused `arm64-assert` run of
`tools/ci/arm64_jit_integer_aggregate_args_contract.sh` passed after these
fixtures were added. The root will run the full four-profile matrix before
integration. The prior nested-HFA four-profile log is
`/Users/frityet/Scratch/ljmt-nested-hfa-20261029/four-profile.log`.
The tested worktree was `/Users/frityet/.codex/worktrees/arm64-math/LuaJITMT`.

The separate ordinary-runtime x86_64 witness that selected this slice remains
the b1d19 baseline recorded in
`notes/arm64-ffi-aggregate-next-gap-audit-2026-10-29.md`: nested two-float
argument, one selected root, two CALLXS nodes, 25 selected-root exits and 2,752
C effects; ARM64 had no selected-root witness there. The production change is
ARM64-only. The current four-profile result validates this implementation's
ARM64 recorder, assembler admission, native entry, exits and GC reuse; it is
not a refreshed ordinary x86_64 corpus run.

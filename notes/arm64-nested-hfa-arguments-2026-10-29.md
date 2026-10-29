# Nested homogeneous float aggregate CALLXS arguments

This checkpoint adds a bounded Darwin ARM64 HFA argument shape: named nested
structs containing two contiguous `float` members. Recursive CType traversal
uses stable snapshots, tracks active type IDs and a depth bound, verifies exact
field offsets and aggregate extents, and preserves the outer by-value CType
identity. The existing independent semantic and post-RA passes continue to
validate the raw XLOAD offsets, source snapshot, CALLXS mapping and physical
FPR carriers from their own walks.

Arrays and anonymous `CTA_SUBTYPE` members remain closed. Nested HFA results
remain closed; the result classifier explicitly rejects nested shapes even
when its caller omits the output descriptor. Unions, references, VLA/vector
members, mixed scalar leaves, padding and over-aligned nested wrappers fail
classification.

The native fixture uses `struct { struct { float lo, hi; } inner; }` and checks
weighted C results against an independently JIT-disabled oracle with a live
threading peer. It observes CALLXS completion and selected-root exits, forces
full GC and re-entry, changes to an alternate C callee to check the guarded
exit, and runs independent semantic/post-RA mutations for each member offset
and the source snapshot. Signature negatives cover nested HFA results, nested
unions, over-aligned nested members and arrays.

## Validation

`tools/ci/arm64_jit_integer_aggregate_args_contract.sh` passed all profiles:
`arm64-assert`, `arm64e-assert`, `arm64-release`, and `arm64e-release`. The
captured run log is `/Users/frityet/Scratch/ljmt-nested-hfa-20261029/four-profile.log`.
The tested worktree was `/Users/frityet/.codex/worktrees/arm64-math/LuaJITMT`.

The separate ordinary-runtime x86_64 witness that selected this slice remains
the b1d19 baseline recorded in
`notes/arm64-ffi-aggregate-next-gap-audit-2026-10-29.md`: nested two-float
argument, one selected root, two CALLXS nodes, 25 selected-root exits and 2,752
C effects; ARM64 had no selected-root witness there. The production change is
ARM64-only. The current four-profile result validates this implementation's
ARM64 recorder, assembler admission, native entry, exits and GC reuse; it is
not a refreshed ordinary x86_64 corpus run.

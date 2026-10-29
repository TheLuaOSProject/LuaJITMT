# ARM64 multi-lane HFA result checkpoint (2026-10-29)

This checkpoint extends the Darwin ARM64 direct HFA result path from at most two
members to three and four `float` members. The argument classifier remains
limited to two members; this is a result-only ABI opening. The classifier still
proves homogeneous fields and their offsets, and the boxed CData result remains
the only Lua-visible value.

## IR and proof shape

`CALLXS` owns a contiguous group of `IR_HIOP` result lanes. Lane zero is the
CALLXS result in D0. Lanes one through three are HIOPs in D1 through D3. Each
HIOP carries a KINT lane tag in `op2`; using the same operands for every HIOP
allowed IR canonicalization to merge lanes, so the tag preserves each lane as a
distinct SSA value. The direct emitter assigns this tag; the register-allocation
hint, assembler result setup, and both admission passes validate it before the
fixed-register mapping is accepted.

Semantic admission independently binds the CALLXS group to the exact result
CType and source call, requires ordered lane tags and stores, checks every
field offset and result type, and rejects every raw lane reference in snapshots.
Post-RA admission repeats those checks on allocated IR and additionally requires
D0-D3 for the corresponding values. The fixture mutates lane tags, lane order,
store bindings, field offsets, and physical registers in each pass.

## Validation

The focused aggregate contract passed all four profiles:

- `arm64-assert`
- `arm64e-assert`
- `arm64-release`
- `arm64e-release`

Log: `/Users/frityet/Scratch/ljmt-hfa-multilane-20261029/four-final/four-profile.log`.
The contract includes selected-root entry, distinct three/four lane values,
forced result-signature exits, full-GC reentry, and foreign-effect accounting.

An ordinary ARM64 build without trace-test helper macros also passed the
portable aggregate probe: 11 rows, 25 oracle comparisons and 2,752 foreign
calls per row. The three-float root published two CALLXS, two CNEWs and six
XSTOREs; the four-float root published two CALLXS, two CNEWs and eight XSTOREs.
Each had a selected-root exit at `pc16:CGET`. Runtime, archive, and helper hashes
were captured with the probe output under
`/Users/frityet/Scratch/ljmt-hfa-multilane-20261029/arm64/`.

The matching ordinary x86_64 build compiled with `CFLAGS=-Werror` and passed
the same 11-row probe, again with 25 comparisons and 2,752 foreign calls per
row. It retained native witnesses for the existing one-eightbyte aggregate
cases; no selected-root witness appeared for the double-pair, triple-float, or
quad-float result rows. That absence is observational evidence, not a proof of
an x86_64 refusal. The x86_64 executable, archive, DSO, strict build log, and
probe output are under
`/Users/frityet/Scratch/ljmt-hfa-multilane-20261029/x64/`.

The earlier two-double checkpoint at base `b93e7d7f0fac87c3f5dcaad58b23657aedc3d130`
also passed the ordinary ARM64/x86_64 probe. It had a selected ARM64 root with
four XSTOREs and 25 exits; x86_64 had no selected-root witness for that row.
Its immutable bundles and original logs are under
`/Users/frityet/Scratch/ljmt-b93-double-pair-20261029/`.

## Boundaries

This checkpoint covers direct homogeneous floating results with one through
four members. It does not expand argument HFA limits, integer aggregate
classification, stack-passed aggregates, aggregate variadic tails, or indirect
aggregate results.

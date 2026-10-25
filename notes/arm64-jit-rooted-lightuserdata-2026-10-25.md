# Exact lightuserdata through rooted ARM64 reads

The generic bounded table reader now admits lightuserdata keys and results in
ordinary experimental ARM64 root loops. Lightuserdata is a 47-bit segmented
token, comprising a segment ID and low address bits. Generated code copies,
hashes and restores that token; it neither dereferences it nor treats it as
a GC edge. The six-type GC classifier is unchanged.

Semantic and post-register-allocation admission separately require exact
guarded LIGHTUD SLOAD/VLOAD provenance, root modes, CARG order, immediate
nonzero status guards, post-helper snapshots and result type guards. Both
passes accept matching PHIs and snapshots. Allocated locations must be GPRs
or complete, aligned, nonoverlapping 64-bit spills. Arithmetic, conversions,
raw pointer/prototype types and unguarded producers remain rejected. The
optimized scalar reader retains its old key domain. This read checkpoint kept
lightuserdata stores, CALLXS and side-trace capabilities closed. Existing-key
stores now have a subsequent
[rooted-store certificate](arm64-jit-rooted-lightuserdata-stores-2026-10-25.md).
No emitter changes are needed.

`t-arm64-jit-rooted-lightud-read-admission.c` seeds synthetic opaque addresses
before opening libraries, including zero and nonzero upper segments. These
addresses are never dereferenced. Its Lua matrix exercises both key segments
with INT, NUM, true, false, LIGHTUD and all six visible GC result classes,
plus keys loaded by an earlier rooted read. LIGHTUD results require an actual
loop-carried PHI. Native results are checked with `lua_touserdata` after exit,
segment-changing replacement and full collection. Missing keys and changed
result types exercise exits. The original negative store loops have been
replaced by the subsequent positive store and invalid-token regressions.

The pressure loop loads twelve LIGHTUD tokens before using them as keys. It
requires one root, no aborts and 19--21 native exits across twenty calls; all
profiles observed twenty. It contains 48 generic read calls across the
unrolled trace and nineteen allocated LIGHTUD spills. Authentic trace clones
independently reject wrong helpers, root aliases, status guards, unsupported
producer/result types, lost guards/snapshots, odd spills and a 32-bit status
overlapping the upper half of a token spill. A separate SLOAD-key clone covers
the argument path.

The complete `tools/ci/arm64_jit_rooted_gc_contract.sh` passed on ARM64 and
arm64e/BTI, assertion and release, including all earlier rooted scalar/GC
read, store, raw nil-deletion and lifetime regressions. Log:
`/tmp/ljmt-lightud-read-contract.log`. The wider experimental/bootstrap, stock
and signed x86-64 regression now passes at the subsequent segment-map runtime
checkpoint `7f377f51`, with a bootstrap-only test-profile correction; see
[combined validation](lightuserdata-validation-2026-10-25.md).

The first pressure draft initialized its snapshot-visible sum as integer zero
before replacing it with a NUM. It observed 620 warm exits, 601 at snapshot
49, despite correct results. The snapshot-only INT SLOAD of the previous sum
does not match later NUM entries. Initializing that stress accumulator as
`0.5` preserves its type and yields the required twenty exits without changing
the computed result or lowering the threshold. This records a separate
dual-number trace adaptation/performance limitation, not a lightuserdata
identity failure. Initial log: `/tmp/ljmt-lightud-read-dev.log`.

This native certificate consumes existing tokens. Concurrent segment creation
has a subsequent, separate
[atomic publication repair](lightuserdata-segment-map-2026-10-25.md); native
read coverage alone does not establish that shared interner property.

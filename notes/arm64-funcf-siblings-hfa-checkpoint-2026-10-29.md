# FUNCF, sibling-side and HFA-result integration

Production source `75c83e708` combines one-member HFA results, a bounded
nonleaf FUNCF return path, and multiple root-linked scalar side traces.
Later test-only commits add three-child middle retirement (`5c90229af`) and
dedicated FUNCF caller-return mutations (`6b16c140c`). These changes are on
`codex/arm64-parity`; main remains unchanged.

The HFA extension preserves the declared aggregate CType while storing a
single raw S0/D0 carrier into its result box. Ordinary ARM64 and x64 builds
both witness retained float/double argument/result loops. The ten aggregate
rows pass 250 comparisons and 27,520 foreign calls per target, including
retained-object checks after collection. Two-float results remain x64-only.
See [HFA results](arm64-hfa-aggregate-results-2026-10-29.md).

The sibling-list extension separates mutable root topology from each child's
immutable generation/exit certificate. Retirement now requires the target
child exactly once in the validated list. Component fixtures cover two and
three children, head/tail/interior retirement, preserved sibling targets,
full GC and native reuse. Root entry uses the existing TG native-entry lease;
capture/publication/retirement use their existing token/SMR protection.
See [multiple sides](arm64-jit-multiple-root-sides-2026-10-29.md).

The nonleaf FUNCF component fixture proves exit-free native returns with a
live peer, collection, exact captured-callee identity exits, restoration and
native reuse. Independent semantic and post-RA clones reject seven malformed
source/return flows each. Its latest dedicated run covers ARM64 and arm64e
assertion profiles, not release profiles.

Integration first exposed a regression in the existing child-frame tests:
`C_BASE_ZERO`, snapshot 2, was accepted by post-RA validation at `5dabe994d`.
The one-result inline-frame relaxation had escaped the new FUNCF scope.
`75c83e708` restores the previous FORL rules and keeps the new replay within
FUNCF. The complete Lua-leaf contract then passes all four ARM64/arm64e
assertion/release profiles, including nested calls, fixed varargs, child exits,
normalized arguments, fixed two-result returns and their negative mutations.
The initial failure and successful rerun are preserved in
`/Users/frityet/Scratch/ljmt-funcf-integrated-20261029/`.

## Ordinary evidence and unresolved gaps

Exact-source ordinary assertion builds at `75c83e708` pass both 509-case
stock corpora on ARM64 and x64, the snapshot-PC fixture, vararg companion,
all 260 workload-inventory comparisons and the ten aggregate rows. The
inventory still has native root-exit witnesses in 18/20 rows on each target.

The ordinary topology matrix does **not** pass as a whole. ARM64 fails the
required `nonleaf-function-root-return` publication assertion; the preserved
binary reproduces it. x64 publishes the intended return root. A diagnostic
copy that disables only that assertion, while retaining all comparisons and
reporting, lets later cases run. The new two- and three-independent-guard FORL
cases have native parent-exit witnesses but no ARM64 children; x64 publishes
two and three distinct children linked to the same root. The passing component
fixtures therefore establish narrower source shapes, not these ordinary
topologies. Both differences remain active implementation work.

The ordinary driver stopped at the ARM64 topology failure. A separate resume
completed x64, and standalone runs completed the remaining ARM64 inventory
and aggregate checks. The resume driver's generic final success line applies
to that x64 run only. Source IDs, immutable runtime/archive/module bundles,
SHA-256 hashes, complete logs and both topology sources are retained under
`/Users/frityet/Scratch/ljmt-funcf-siblings-hfa-integration-20261029/ordinary/`.

The latest complete experimental gate remains `b549a895b`; no complete gate
or full parity claim is made for this newer integration.

## Test-profile preflight difference found

An isolated build split at `75c83e708` proves that adding only
`LJ_TRACE_TEST_HELPERS` makes the read-only captured FUNCF probe pass, while
adding only `LUAJIT_MCODE_TEST` does not. Source audit found three diagnostic
exceptions introduced by `b39f6fd03f`: helper builds did not unconditionally
reject an invalid root generation, an out-of-prototype PC, or failed Lua-frame
proof. The frame proof still required FORL and rejected the new FUNCF child
frame in ordinary builds. The exceptions masked that missing proof.

The strict-preflight repair `498f2b098` is integrated separately from the
FUNCF frame extension. All build profiles must now enforce identical checks;
the missing FUNCF proof remains active work. Earlier helper-only positives
and gate passes are historical execution results under those weaker checks,
not evidence that ordinary recorder preflight admitted the same traces.
The ordinary stock/inventory/aggregate results above are unaffected by this
test-only bypass removal. Helper-profile runtime/archive copies, split build
logs, source IDs and hashes are in the `helpers-arm64/` and `profile-split/`
subdirectories of the integration evidence directory.

## Strict FUNCF follow-up

`6968c4cac` adds the exact FUNCF child-frame/completed-return path and tests
both mutable and read-only local captures. The original source/PC/owner and
frame checks remain unconditional. The component fixture passes ARM64 and
arm64e assertion builds with five exit-free native returns, one exact identity
exit, fourteen semantic and fourteen post-RA mutation refusals, and two
invalid-loop-reference refusals per profile.

After the diagnostic bypass removal, the full Lua-leaf contract independently
passes all four ARM64/arm64e assertion/release profiles. The log is
`strict-preflight-lua-leaf.log` at the integration evidence directory.

Ordinary assertion builds at exact source `6968c4cac` pass both 509-case stock
corpora, snapshot-PC/vararg companions, the twenty-case inventory, ten-case
aggregate probe and all topology result/error comparisons on ARM64 and x64.
The previously failing read-only nonleaf root now publishes on ordinary ARM64
with a return trace and 81 selected-root type-error exits. Strict ARM64/arm64e
GDBJIT trace/assembler compilation passes. Logs, source ID and immutable
runtime/archive/fixture bundles are in `ordinary-fixed/`; the failed older
run remains in `ordinary/`.

The independent-guard FORL child-publication difference remains: these rows
still have x64-only side witnesses despite matching results.

## Multiple-child fixture repair

The full gate at production source `6968c4cac` reached scalar side adaptation
and stopped at `t-arm64-jit-side-adaptation.c:182`. The old fixture required
exactly one child. The overflow case now publishes two valid children: a
self-linked loop and another child linked to the root. The recorded topology
was root 1, head 3, then child 2; the fixture rejected it before inspecting
either child.

The fixture now selects the named kernel's exact prototype, walks the bounded
sibling list, checks every child's root identity and native parent-exit edge,
and selects the intended self-linked loop. It retains native-entry/exit bounds,
copy-mutation checks, full collection, flush/reuse and type-error checks.
The focused contract passes with the same runtime/archive and production
source. This is a test correction, with no runtime admission change.

The original gate source/log are `full-gate-source.txt` and `full-gate.log`.
The failed archive/runtime, original fixture source, a diagnostic fixture
binary/source, topology log and successful focused rerun are preserved in
`full-gate-failure/`. The original temporary fixture executable had already
been removed by contract cleanup; the preserved diagnostic executable was
relinked against the saved archive. A continuation from the failed contract
will complete the remaining gate on unchanged production source. No complete
new gate pass is recorded yet.

The first continuation passed through native-loop and exit contracts, then
stopped in the root-entry source-order check. Multiple-child support moved
the per-child validation into `trace_root_entry_side_view_acq`; the source
check still searched the list wrapper for those operations. It now inspects
the per-child function and separately requires the bounded sibling walk,
null terminator, unchanged parent topology, and nonallocating entry path.
The corrected root-entry contract passes ARM64 and arm64e assertion profiles
plus ARM64 release, including the repaired side-adaptation fixture. Output
is in `root-entry-sibling-contract.log`; `resume-full-gate.log` preserves the
first continuation. This second repair also changes tests only.

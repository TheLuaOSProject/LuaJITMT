# ARM64 Lua leaf comparison branches (2026-10-28)

ARM64 Lua leaf admission now proves a bounded single-comparison, acyclic
two-arm control-flow graph. The source graph is copied from the prototype's
bytecode, requires forward-only edges and a shared join, and binds the source
comparison to the recorded numeric guard, operand order, polarity, and exact
cold-edge snapshot footer. Numeric comparison matching retains Lua's NaN
unordered behavior. The semantic and post-RA passes independently validate
the comparison, snapshot maps, source-path state, and selected-path liveness.

The bounded recognizer accepts a single comparison and straight-line arms
composed from the leaf's already-supported pure operations. It rejects nested
comparisons, cycles, backward jumps, early-return paths, and a resume before
the comparison. This is a conservative leaf subset, not general Lua CFG
support.

The C contract exercises all four ordered comparison bytecodes, reversed
operands, positive and negative zero, a finite-warmed trace taking a NaN cold
exit, both warm-up branch directions, primitive overwrites, and a divergent
arm case where one path copies its parameter while the other assigns a
constant. Its malformed-map mutation removes that live parameter from the
branch-edge snapshot and is rejected independently by semantic and post-RA
admission. This verifies live-parameter rejection, but is not a counterfactual
reproduction of the earlier serial-arm liveness bug. The ordinary Lua
companion checks publication and same-trace reuses; it makes no
native-execution claim.

Fixture exploration also found current closed cases: an `if` with an empty
arm and a two-arm variant with multiple arithmetic operations did not publish
in the focused run. The empty-arm case lies outside this two-nonempty-arm CFG
shape; multi-operation arm admission remains unverified. No broader arithmetic
branch parity claim is made here.

Focused validation on 2026-10-28 used one ARM64 assert profile with the return
exit C fixture and ordinary Lua companion. It passed with 95 C cases, 392
native runs, 95 terminal exits, 297 same-trace reuses, 95 GC reuses, and
semantic/post-RA negative totals of 1552/1561. The ordinary companion passed
54 publications, 15 numeric ranges, and 138 same-trace reuses.

The complete leaf contract then passed on ARM64 assert, ARM64e assert, ARM64
release, and ARM64e release. Each profile reported the same return-exit C
counts above. Full logs and the recorded script exit status are in
`/Users/frityet/Scratch/ljmt-arm64-branch-20261028/`.

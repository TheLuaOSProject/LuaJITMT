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

The bounded source-path proof also admits a two-arm case with different
multi-operation arithmetic bodies. Its terminal child snapshot retains a
trailing slot beyond the selected path's live frontier. For branch snapshots,
semantic and post-RA checks allow the source-bounded `nslots` extent up to the
callee frame, with snapshot geometry bounded by `topslot + 1 + LJ_FR2`; every
mapped child slot still must be unique and match the independently replayed
source value. Non-branch child snapshots retain exact `nslots` equality. C
mutations reject a changed returned `SUBOV`, a wrong reference in a high child
map entry, a duplicate slot, and an out-of-frame slot in both passes. These
map mutations exercise source binding and slot validation; they do not
independently classify a retained mapped value as dead. An empty-arm `if`,
nested comparisons, cycles, backward jumps, and early returns remain outside
the admitted source grammar.

Final validation on 2026-10-28 passed the complete leaf contract on ARM64
assert, ARM64e assert, ARM64 release, and ARM64e release. Each profile reported
96 return cases, 397 native runs, 96 terminal exits, 301 same-trace reuses, 96
GC reuses, and semantic/post-RA negative totals of 1578/1588. The ordinary Lua
companion reported 55 publications, 16 numeric ranges, and 142 same-trace
reuses per profile. The dynamic-FORL recorder and IR-admission contracts also
passed; the latter records synthetic integer unsigned guards as a type
rejection after exact induction-candidate selection. Logs and exit statuses
are in `/Users/frityet/Scratch/ljmt-arm64-branch-20261028/`.

# ARM64 dead numeric DAGs in zero-formal CALLM traces

The post-register-allocation scalar proof now admits a retained numeric
expression cone when the cone has no machine carrier and cannot affect a
snapshot. This covers the x64-native zero-formal `CALLM` case where a producer
returns a numeric expression plus two values, while the consumer takes no
formals and returns a fixed number. LuaJIT can retain the producer arithmetic
in trace IR after DCE has removed its machine code.

The proof starts only when a missing-location value has an unlocated,
unspilled `IRT_NUM` ADD, SUB, MUL, or DIV user. It walks the transitive SSA use
cone with a fixed bitmap over the IR reference domain. Every reachable user
must also be one of those unlocated pure numeric operations. Any allocated or
spilled user, other operation, snapshot reference, or trailing rename rejects
the cone. The scan is attempted lazily from that candidate path; ordinary
fusion checks retain their existing fast path. Single dead terminal arithmetic
keeps its prior bounded rule, now with explicit snapshot and rename checks.

The C contract verifies a real retained `source -> MUL -> ADD` chain. Its
postRA mutation assigns the ADD a register while leaving the source and MUL
without carriers; this must fail even though ARM64 can fuse an MUL into an
ADD. A separate CALLM result-map mutation points the returned numeric slot at
a dead DAG node and must fail both semantic and postRA admission. The selected
root executes native exits across a full GC with a live MT peer, and the test
checks the same trace generation, code address, size, and copied code bytes
afterward.

## Validation

`tools/ci/arm64_jit_callm_dead_numeric_contract.sh` runs the C admission and
native fixture plus the ordinary Lua companion in ARM64 and ARM64e assert and
release profiles. The Lua companion requires selected roots and selected-root
exits for a dead terminal, a three-operation chain, and division after GC.
The same companion also passed with the ordinary ARM64 assert runtime restored
without test-helper flags; its output is
`/Users/frityet/Scratch/ljmt-callm-dead-numeric-20261029/ordinary-arm64.log`.
Matched ordinary x64 topology evidence for the chain and division is in
`/Users/frityet/Scratch/ljmt-side-ancestry-20261029/discarded-expressions-x64.log`;
the original matched probe source is
`/Users/frityet/Scratch/ljmt-side-ancestry-20261029/discarded-expression-probe.lua`.

The C negative mutates only private IR/snapshot copies. This remains limited
to pure numeric ADD/SUB/MUL/DIV cones; live operations and other IR kinds stay
closed. A follow-up private postRA mutation gives the terminal ADD an aligned
two-word spill at slot 4 with the matching 16-byte spill area. Admission
rejects it; a temporary counterfactual without the `ra_hasspill` check in the
dead-node predicate admits the same mutation. The test also replaces the
fixture's final NOP with a well-formed trailing RENAME. A control RENAME to an
already located unrelated NUM ref passes, while renaming a reachable dead-DAG
user is rejected; removing only the DAG suffix-rename scan admits that case.
The native source trace itself ends in NOP, so the rename cases are private
proof mutations rather than claims about a naturally occurring trace shape.

The focused one-profile ARM64 assertion runs for the new mutations passed at
`/Users/frityet/Scratch/ljmt-callm-dag-rename-negative.log`. The two isolated
counterfactual failures are preserved at
`/Users/frityet/Scratch/ljmt-callm-dag-spill-counterfactual.log` and
`/Users/frityet/Scratch/ljmt-callm-dag-rename-counterfactual.log`.

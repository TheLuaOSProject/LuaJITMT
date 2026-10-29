# ARM64 open CALLM consumer identity exits

The open-result CALLM contract already proved producer identity exits and
open-vector admission, but did not force a mismatch at the consumer's own
function identity guard. The new focused fixture uses a parameterized consumer
so the same published loop root can run first with consumer A and then
consumer B. It covers one, two, and three results from an adjacent open
producer `CALL B=0`, followed by `CALLM B=2,C=0`.

For every result count, the fixture finds the CALLM identity guard by both its
exact snapshot footer PC and the guarded consumer function constant. It checks
that the guard snapshot contains a restorable numeric entry for every open
producer result at the CALLM argument slots. Calling the same selected root
with consumer B then proves the native exit number equals this exact guard
snapshot, after the producer result vector is available. The independently
JIT-disabled oracle uses consumer B as well; equality of the completed result,
plus restored `base`, `top`, `cframe`, native-base and native-state checks,
covers the interpreted continuation and frame/MULTRES restoration.

After the first exit, the fixture switches back to consumer A and confirms
native entry into the same trace. It repeats the consumer-B exit and consumer-A
native reentry after a full collection, with the MT peer still live. The trace
remains runnable with the same mcode pointer and size throughout. The ordinary
Lua companion checks the one/two/three-result semantic oracle with both
consumers; it makes no native-execution claim.

Validation: `tools/ci/arm64_jit_open_callm_exits_contract.sh` passed ARM64 and
arm64e assertion and release profiles. Each profile passed three runtime
shapes and three ordinary oracle cases. The exact log is
`/Users/frityet/Scratch/ljmt-open-callm-consumer-exit-20261029/focused-4.log`.
The fixture is parameterized-consumer coverage; it does not claim a mutable
captured-cell consumer path.

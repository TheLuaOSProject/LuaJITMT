# ARM64 captured open-CALLM consumer arguments

The open-result `CALLM` proof now handles a mutable consumer captured by the
loop runner when its producer call is repeated inside the loop. The producer
function itself remains bound to one unchanged formal argument and its exact
guarded leaf identity. For each current-half consumer snapshot, semantic and
post-RA admission independently select the latest same-iteration root snapshot
before the producer call, replay its bounded straight-line source interval,
and bind every actual producer argument to that iteration's values. The
existing child leaf replay then binds the complete returned vector to the
consumer's exact saved result window.

The source replay is intentionally limited to CGET/MOV, UGET, KSHORT/KNUM, and
numeric VV/VN/NV add, subtract, multiply, and divide. It requires a root-frame
map with unique, in-frame slots and valid earlier refs; the guarded producer
function must come from an unchanged formal. Unsupported bytecodes, effects,
or source shapes remain closed. Calls before the loop retain the prior
same-half proof.

The C fixture covers producers returning one, two, and three values. For each
shape it requires the selected JFORL root to take the exact consumer-identity
exit after the captured consumer changes, verifies source/IR/map corruptions
are rejected independently by both validators, then restores the consumer
and checks reuse of the same mcode generation across full GC and reentry with
a live threading peer. The ordinary Lua companion checks interpreted results
against a separate oracle while swapping the captured consumer.

Validation: `tools/ci/arm64_jit_open_callm_exits_contract.sh` passed on the
frozen source for arm64-assert, arm64e-assert, arm64-release, and
arm64e-release. Every profile passed all three captured shapes and their
source, IR-reference, pre-loop-value substitution, and mapped-output
negatives. Exact selected native exit number 5 was observed for each shape;
the unchanged root/mcode was reused after GC. The final log is
`/Users/frityet/Scratch/ljmt-callm-consumer-20261029/final-four-profile-contract.log`.

This extends the bounded CALLM leaf grammar; it does not establish support for
arbitrary captured calls, variable producer arity, or general effects in the
replayed source interval.

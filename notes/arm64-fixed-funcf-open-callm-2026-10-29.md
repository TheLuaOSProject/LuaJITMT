# Fixed FUNCF roots with adjacent open CALLM — 2026-10-29

This checkpoint admits one straight-line fixed-arity `FUNCF` root that calls a
one-argument producer with `CALL B=0`, immediately forwards its 1–3 returned
scalar values to a one-result `CALLM B=2,C=0` consumer, then computes a scalar
`RET1`. The producer and consumer must be exact function parameters copied
into their bytecode function slots. Existing function-identity guards and
child-frame snapshot proofs bind each closure; the producer’s returned values
must exactly fill the consumer argument window. The bytecode grammar, semantic
replay, and allocated post-RA replay remain separate admission checks.

The narrow post-RA exception for a root without `IR_LOOP` is enabled only when
the root starts at `BC_FUNCF` and its bytecode passes this exact open-CALLM
source recognizer. The same no-loop case remains rejected for loop-rooted
traces and other FUNCF source shapes. Raw `CALLM` results are never exposed as
Lua values: the proof follows the source values into the consumer and then
binds the consumer’s single result to the caller’s `RET1` map.

`tests/t-arm64-jit-funcf-calls.c` exercises three separate candidate prototypes
with producers returning one, two, and three values. The consumers use one,
two, and three formals respectively. A separate JIT-off oracle producer and
consumer are used for each candidate. The roots have a live threading peer;
each passes oracle comparison, executes a selected root natively before and
after full GC, and retains identical runnable mcode across collection. The
three-result case changes the producer closure, observes the exact first
producer identity exit from the candidate root, then verifies native re-entry
with the original closure.

For each arity, private trace/prototype copies first pass both admission walks,
then independently reject a changed `CALLM` arity and a terminal snapshot whose
`RET1` value is replaced by the input parameter. On the focused contract the
existing totals increased from 28/29 to 34 semantic/post-RA negatives. No
production diagnostic output is present.

Validation: `tools/ci/arm64_jit_funcf_record_contract.sh` passed with assertions
enabled and disabled, for both arm64 and arm64e (the release builds retain the
repository’s pre-existing `lj_api.c:879` unused-variable warning). Each
FUNCF-call fixture run reported 23 native returns, 4 identity exits, 34
semantic negatives, and 35 post-RA negatives. Logs:

- `/Users/frityet/Scratch/ljmt-open-callm-root-20261029/fixed-root-assert-final.log`
- `/Users/frityet/Scratch/ljmt-open-callm-root-20261029/fixed-root-release-four-profile.log`

Scope is deliberately limited to the fixed root, one producer CALL, one
adjacent consumer CALLM, and one scalar return continuation. It does not admit
captured-function root bindings, consumer returns with multiple values,
branches, heap effects, or other call/return topologies. The two-result native
positive uses two NUM values; a separate mixed INT/NUM producer shape did not
publish in this fixture and remains unclaimed.

Function identity and output ordering: a producer result that requires child
IR computation must be ordered after the producer's identity guard in both
independent semantic and post-RA walks. A result that is exactly a supplied
argument reference or scalar KINT/KNUM constant may predate that guard because
it does not execute a value-producing child operation; the computed one- and
two-result producers and direct-forwarded three-result producer exercise both
cases. This distinction preserves the direct-return behavior while preventing
an arithmetic result from being authorized only by a later function guard.

Final guard-order recheck logs:

- `/Users/frityet/Scratch/ljmt-open-callm-root-20261029/final-recheck.log`
- `/Users/frityet/Scratch/ljmt-open-callm-root-20261029/final-release.log`

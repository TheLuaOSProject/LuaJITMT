# Zero-formal producers in captured open CALLM — 2026-10-29

Captured open-CALLM consumer admission now supports zero-formal producer
functions for one or two fixed numeric results. When the producer has no
arguments, current-half replay does not require a fresh post-LOOP snapshot for
an argument vector: there is no vector to rebuild. Instead, semantic and
postRA checks require the producer function value to be the exact guarded
`SLOAD` of an unchanged runner formal, the producer CALL to be exactly
`B=0,C=1`, and the child evaluator to bind every returned value into the
CALLM continuation map. Nonzero producer arities still use their existing
same-half argument replay helpers.

The native fixture covers one and two numeric results with a setter-mutable
fixed consumer. Each case requires the exact consumer-identity exit, independent
semantic and postRA rejection of malformed producer CALL arity, altered
function-source map and SLOAD operand, and altered result-map value. It then
checks native reentry and unchanged code generation across full GC with a live
threading peer. The ordinary companion checks the interpreted oracle while
swapping the captured consumer.

The three-result zero-formal producer remains closed. Its exact caller has
`CALL A=10,C=1` and a fixed frame size of 12, so its third open result is
outside that frame. Recorder and admission source gates both require the open
result window to fit the declared frame. This checkpoint keeps that bound
strict; extending transient CALLM stack extent requires a separately derived
VM, snapshot and physical-stack proof.

Matched ordinary probes use the saved ARM64 binary at source
`0f3bd3286ea5d754bf14483ef52ec6ddc76645dc` and saved x86_64 binary at
`ee12f50e9c4f660d79ea4cd39e84018f446ef933`. The same Lua source reports
x86_64 JFORL roots 1/2 for one/two/three-result cases, while ARM64 has no
selected root in any case. Results match across targets. The ordinary probe
observed root publication before the consumer swap but no texit event for that
root; exact selected consumer exit 5 is established by the native C fixture on
ARM64. The probe and outputs are preserved in
`/Users/frityet/Scratch/ljmt-zeroformal-producer-20261029/`.

`tools/ci/arm64_jit_open_callm_exits_contract.sh` passed on arm64-assert,
arm64e-assert, arm64-release and arm64e-release, including the existing
captured producer tests. The frozen-source log is
`/Users/frityet/Scratch/ljmt-zeroformal-producer-20261029/four-profile-contract.log`.

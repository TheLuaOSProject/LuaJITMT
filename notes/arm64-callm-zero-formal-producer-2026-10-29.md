# Zero-formal producers in captured open CALLM — 2026-10-29

Captured open-CALLM consumer admission now supports zero-formal producer
functions for one, two, or three fixed numeric results. When the producer has no
arguments, current-half replay does not require a fresh post-LOOP snapshot for
an argument vector: there is no vector to rebuild. Instead, semantic and
postRA checks require the producer function value to be the exact guarded
`SLOAD` of an unchanged runner formal, the producer CALL to be exactly
`B=0,C=1`, and the child evaluator to bind every returned value into the
CALLM continuation map. Nonzero producer arities still use their existing
same-half argument replay helpers.

The native fixture covers one, two, and three numeric results with a setter-mutable
fixed consumer. Each case requires the exact consumer-identity exit, independent
semantic and postRA rejection of malformed producer CALL arity, altered
function-source map and SLOAD operand, and altered result-map value. It then
checks native reentry and unchanged code generation across full GC with a live
threading peer. The ordinary companion checks the interpreted oracle while
swapping the captured consumer. The three-result case also rejects `C=2`, a
truncated snapshot slot extent, and a shifted producer/consumer register window
in both admission passes. The fixture passed three separate fresh-process
arm64-assert runs with 25 native entries, 134 semantic negatives, and 142
postRA negatives per run.

The new bound is deliberately narrow. The producer must be the exact adjacent
`CALL B=0,C=1` / `CALLM B=2,C=0` pair, with source-derived register positions,
one producer result, a fixed consumer argument vector of at most three values,
and all extents below `LJ_MAX_JSLOTS`. Only the transient result window may
extend past the caller's declared frame, by at most `LJ_FR2`; the consumer's
own normal result extent remains within its frame. Both semantic and postRA
passes independently check the pair, frame and mapped result vector.

No `snap_restore` change was needed. The admitted three-result fixture's CALLM
snapshot has `topslot == framesize`; its extra transient map entry remains in
the reserved VM stack area. Lua frame entry reserves the declared frame plus
`LJ_STACK_EXTRA`, so recursion probes that stayed within valid Lua frames did
not reach snapshot-restore stack growth. We removed the attempted recursion
probe instead of claiming it exercised that branch. `LJ_FR2` is a build
configuration property, so the bound remains expressed in the macro rather
than architecture names.

Pre-change ordinary probes use the saved ARM64 binary at source
`0f3bd3286ea5d754bf14483ef52ec6ddc76645dc` and saved x86_64 binary at
`ee12f50e9c4f660d79ea4cd39e84018f446ef933`. The same Lua source reports
x86_64 JFORL roots 1/2 for one/two/three-result cases, while ARM64 has no
selected root in any case. Results match across targets. The ordinary probe
observed root publication before the consumer swap but no texit event for that
root; exact selected consumer exit 5 is established by the native C fixture on
ARM64. The probe and outputs are preserved in
`/Users/frityet/Scratch/ljmt-zeroformal-producer-20261029/`.

`tools/ci/arm64_jit_lua_leaf_contract.sh` and
`tools/ci/arm64_jit_open_callm_exits_contract.sh` passed on arm64-assert,
arm64e-assert, arm64-release and arm64e-release. The ordinary leaf companion
publishes and reuses its zero-formal three-result case on all four profiles.
The x86_64 assertion build also passed a 5,000-iteration three-result CALLM
sum with full collection and repeat-result equality. Logs are in
`/Users/frityet/Scratch/ljmt-zeroformal-producer-20261029/`.
On x86_64 the new zero-formal ordinary case also published and reused on its
first attempt; the ARM-oriented whole companion later stops at its upvalue
refusal assertion, so that is not counted as a full x86_64 companion pass.

# Returned-call C1 triplet reachability

The `trace_arm64_lua_leaf_returned_call()` C1 three-result extension is not
reachable for the ordinary open-result producer/consumer source shape under the
current ARM64 source grammar. Raising its result-frame bound alone would not
admit that program.

## Discriminating source and target evidence

The probe in `Scratch/ljmt-returned-call-c1-20261029/c1-noobserver.lua` uses:

```lua
local producer = function() return 1.25, .25, .5 end
local consumer = function(a, b, c) return a + b + c end
local mid = function(x)
  local y = consumer(producer())
  return y + .125
end
local root = function(x)
  local y = mid(x)
  return y + .75
end
```

With a bytecode field decoder using LuaJIT's `A/C/B` encoding, `mid` has the
relevant adjacent pair `CALL A=3 B=0 C=1` then `CALLM A=1 B=2 C=0`. This is the
shape tested by the returned-call helper: an open-result producer call followed
by a one-result consumer call.

On the frozen x86_64 runtime (`dae24b46aad9d4fc1d36cdce5b94d49d05d4743a`), the
observer-free run returns `28750` and publishes three traces (`nins=7, 9, 13`).
A separate x86_64-only `jit.dump` attribution run shows the
`mid` trace starting at its function body and stopping with a native return.
The dump is attribution evidence only; the result and trace count came from the
observer-free run.

On ARM64 at `bc0313318b2bf983cabbb5d7932309c7ce043bc4`, the same observer-free
program returns `28750` and publishes only one trace (`nins=7`). A temporary,
bounded diagnostic around `trace_arm64_lua_leaf_returned_call()` captured its
first 20 calls. Every call was at the `mid` function entry (`pc == proto`), and
was rejected by the existing requirement that a returned-call continuation
must be a non-entry PC. No call reached the continuation decode or its C1
result-span predicate. The temporary diagnostic was removed before the clean
ARM64 run; no production source change remains.

## Why the continuation is blocked earlier

The current root FUNCF checker, `trace_root_funcf_shape()` in `src/lj_trace.c`,
delegates to `arm64_ir_funcf_bytecode()` in
`src/lj_asm_arm64_admit.h`. That grammar permits one fixed-result `BC_CALL`
with `C=2`, followed by its bounded scalar continuation; it rejects
`BC_CALLM`. Thus `mid` cannot be admitted as a FUNCF root.

When `mid` is an intermediate frame below `root`, the frame proof also requires
`lj_trace_arm64_lua_leaf(mid)`. Its leaf source recognizer rejects `BC_CALLM`,
so the trace cannot proceed from `mid` entry into the producer-return
continuation either. Consequently, the `open_producer` result-span branch in
`trace_arm64_lua_leaf_returned_call()` is downstream of two closed source
paths for this workload.

The appropriate future parity slice is bounded FUNCF/intermediate support for
the paired `CALL B=0,C=1` + `CALLM B=2,C=0` source, with its independent IR,
snapshot and post-RA proofs. The existing C1 span adjustment should only be
changed if that broader source path becomes admitted and a live native witness
then demonstrates the remaining refusal. This note does not claim that all
other returned-call geometries are unreachable.

## Reproduction artifacts

The exact scripts, outputs, diagnostic excerpt, build log, binary hashes, and
runtime source identity are in
`/Users/frityet/Scratch/ljmt-returned-call-c1-20261029/`. ARM64 validation used
Darwin arm64 with `LUAJIT_MT_ARM64_BOOTSTRAP`,
`LUAJIT_MT_ARM64_JIT_EXPERIMENTAL`, and `LUA_USE_ASSERT`; its ordinary run had
no trace observer or test helper. The x86_64 runtime was the frozen ordinary
bundle in `Scratch/ljmt-side-ancestry-20261029/dae-x64/`.

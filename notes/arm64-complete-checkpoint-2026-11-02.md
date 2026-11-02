# November 2 complete ARM64 checkpoint

The combined gate passed at `e0da9e64e` on
`codex/arm64-parity-20261102`. Its production source is identical to
`2387b3ed69dc37db4f0c831715610c360e49c6f1`; the additional commits merge the
previous parity branch's notes and add the fresh route audit. The checkpoint
includes the guarded CHAR formatter, retained integer modulo, and numeric
unary leaf changes that previously had only focused evidence.

## Complete gate

On native Apple Silicon, with Xcode 27.2, macOS SDK 27.2 and deployment target
13.0:

```sh
env JOBS=4 LJ_TEST_REQUIRE_NATIVE=1 \
  sh tools/ci/arm64_jit_fail_closed_gate.sh
```

The process exited 0 and printed the final `arm64_jit_fail_closed_gate OK`
marker. This includes scalar/loop/function entry, independent IR and allocated
layout checks, rooted heap effects, Lua leaf/open-result calls, first-side
entry and lifecycle contracts, all four M7 FFI profiles (ARM64 and arm64e,
assertion and release), and actual VM/recorder safepoint runtime tests.

Log: `/Users/frityet/Scratch/ljmt-parity-checkpoint-20261102/full-gate.log`.
SHA-256: `d2f7a48b3cabfc0e3d4aa5c706ba851e5701123efd107e1ad21d17dd48814a59`.

At this revision the gate finishes with its linked-fixture helper build. An
explicit clean ordinary ARM64 assertion rebuild followed, using only
`LUAJIT_MT_ARM64_BOOTSTRAP`, `LUAJIT_MT_ARM64_JIT_EXPERIMENTAL` and
`LUA_USE_ASSERT`. Its runtime SHA-256 is
`002e70c33653d210dfa304d09d68ee797a18d778e352573cf4744b60962a5667`.
The restoration log is `ordinary-arm64-build.log` in the same directory.

## Ordinary compatibility and bounded concurrency

Both the restored ARM64 assertion runtime and the frozen signed x64 release
comparison runtime pass 509 stock source tests and 509 original-v2 bytecode
imports. The x64 production source is also `2387b3ed6`; its executable SHA-256
is `2e040da0f5d557fc5c81aa137a78115417fd22bd1216d02f346091b1de83e5e9`.
The original-bytecode compiler is `/opt/homebrew/bin/luajit`, version
`2.1.1788856981`, SHA-256
`8f0166d1a31d0ba2318357120d290ff2868612f8e03a8291e736711f6384981c`.

The frozen ordinary ARM64/x64 release comparison builds also pass:

- 1,000 sequential spawn/join cycles with the JIT disabled.
- JIT flush/GC stress with 3 workers, 16 rounds, 32 short-lived threads, and
  2 GC2 workers.

Logs are in the checkpoint directory as `ordinary-{arm64,x64}-stock-source.log`,
`ordinary-{arm64,x64}-stock-v2.log`, `{arm64,x64}-threading-stress.log`, and
`{arm64,x64}-jit-flush-gc-stress.log`. These bounded runs do not prove every
weak-ordering or long-running concurrency scenario.

## Remaining parity work

The broader topology comparison still exposes native call-chain, recursive,
and captured/stateful iterator gaps even though the sampled broad inventory
passes 260 oracle comparisons on each architecture. Prototype-bound
child-return certificate work does not yet demonstrate publication and
execution of a child-to-caller side trace. FFI admission remains bounded by
specific supported ABI classes. No full x86_64 parity claim is made.

Pointer-containing, larger, and array-containing FFI results are developed
on a separate branch after this frozen source point. Their focused evidence
must be recorded separately when integrated; this complete-gate result does
not cover those later changes.

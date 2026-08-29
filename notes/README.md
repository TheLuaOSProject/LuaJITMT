# LuaJITMT ARM64 port status

This directory contains only the live status and durable design constraints.
The 938 append-only investigation and checkpoint notes were moved to
[`archive/`](archive/README.md) so historical observations are still available
without looking like current claims.

## Current checkpoint

Development branch: `codex/aarch64-macos-port`.

Last functionally verified source checkpoint: `2fd6fb49` (2026-08-28). The
documentation archive cleanup does not change functional source. Any later
minimal-divergence source cleanup must be independently reviewed and
revalidated before this line is advanced.

ARM64 remains explicitly opt-in with `LUAJIT_MT_ARM64_BOOTSTRAP`. Native JIT
work additionally requires `LUAJIT_MT_ARM64_JIT_EXPERIMENTAL`. These flags are
intentional: the port has real native execution evidence, but it is not yet a
general ARM64 LuaJIT backend.

Implemented and exercised on Apple Silicon macOS:

- the lockless interpreter bootstrap, TG-local state, safepoints, frame/root
  publication, protected calls, metamethods, selected table operations, and
  interpreter FFI/callback lifecycle;
- ARM64 and arm64e/BTI trace publication, authenticated exits, retirement,
  flush/reuse, GDB JIT preparation, and bounded root/first-side entry paths;
- constant-step integer `FORL`, bounded integer and mixed numeric `LOOP`
  roots, fixed-half and dynamic-step numeric roots, literal-true `FUNCF`, and
  exact all-parameter `ADD_LT`, `ADD_LE`, `ADD_GT`, `ADD_GE`, `SUB_GT`,
  `SUB_GE`, `MUL_LT`, `MUL_LE`, and `DIV_LT` loop profiles; and
- callback-result lifetime across post-detach TG reclamation.

The newest `DIV_LT` profile is deliberately narrow:

```lua
while x < limit do
  x = x / divisor
end
```

It requires the exact all-parameter bytecode and spill-free IR/register shape.
Its compiler proof exhausts 576 profile/arithmetic/guard combinations at each
of the semantic and post-register-allocation gates and admits exactly nine
total profiles. Its runtime proof checks noncommutative FDIV operands, exact
ARM64/arm64e instruction words, type exits, equality boundaries, NaN,
infinities, signed zero, STOPREQ cleanup/reuse, and adjacent no-trace shapes.

## Verification at `2fd6fb49`

- `tools/ci/arm64_jit_fail_closed_gate.sh`: passed in full.
- Native ARM64 vendored LuaJIT suite: `509 passed`.
- Focused all-parameter numeric contract: direct plus two randomized runs on
  ordinary ARM64 and arm64e/BTI; all six executions passed.
- Twelve independent native processes published exactly trace 1 with
  `link=1` and `linktype=loop`, including genuine-NUM `MUL_LT`, `MUL_LE`, and
  `DIV_LT` operands.
- Disposable thin x86_64 build: platform smoke passed under Rosetta with
  `jit.os=OSX`, `jit.arch=x64`, `trace=1`, `link=1`, and `linktype=loop`; the
  x86_64 vendored suite also reported `509 passed`.
- The restored primary artifacts are thin ARM64, `src/lj_asm.o` is newer than
  its source and byte-identical to the archive member, and the worktree was
  clean and synchronized with `origin` before cleanup.

The recurring unused `ccall_rawchild_wait` warning remains pre-existing. The
isolated x86_64 configuration also emits the known discarded non-GC64 probe
diagnostic and unused `topofs` warning before its successful GC64 build.

## Still closed or incomplete

- General ARM64 IR admission, arbitrary Lua programs, and unrestricted spills
  or register layouts.
- General side traces, side-of-side traces, and stitches. Only explicitly
  certified first-side shapes are open.
- Uncertified conversions, calls, allocations, heap effects, table/upvalue
  JIT fast paths, and traced generic FFI calls.
- Full parity with every x86-64 lockless VM, JIT, FFI, profiler, unwind, and
  stress path.
- The complete sanitizer, weak-memory, sustained concurrency, and performance
  recovery matrix from the original roadmap.

Do not describe this branch as a completed ARM64 port yet. A passing stock
suite proves an important compatibility boundary, not general JIT coverage.

## Primary checks

```sh
env MACOSX_DEPLOYMENT_TARGET=13.0 tools/ci/arm64_jit_fail_closed_gate.sh
env LUA=src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet
```

Use focused contracts while developing and run the complete gate before a
checkpoint claim. Keep coherent source, compiler-certificate, runtime-proof,
and documentation changes in separate commits and push each validated slice.

See [`architecture-constraints.md`](architecture-constraints.md) before
changing the VM, JIT, FFI, GC2, publication, or cleanup paths.

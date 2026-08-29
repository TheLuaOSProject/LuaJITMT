# LuaJITMT ARM64 port status

This directory contains only the live status and durable design constraints.
The 938 append-only investigation and checkpoint notes were moved to
[`archive/`](archive/README.md) so historical observations are still available
without looking like current claims.

## Current checkpoint

Development branch: `codex/aarch64-macos-port`.

Last fully verified checkpoint: `2b2868f7` (2026-08-28). Its production source
was introduced at `6d94a92d`; the later commits add independent compiler and
runtime certificates plus the umbrella-gate update. It completes the exact
strict/inclusive ascending and descending division family without opening
adjacent division shapes.

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
  `SUB_GE`, `MUL_LT`, `MUL_LE`, `DIV_LT`, `DIV_LE`, `DIV_GT`, and `DIV_GE`
  loop profiles; and
- callback-result lifetime across post-detach TG reclamation.

The four division profiles are deliberately narrow:

```lua
while x < limit do
  x = x / divisor
end

while x <= limit do
  x = x / divisor
end

while x > limit do
  x = x / divisor
end

while x >= limit do
  x = x / divisor
end
```

They require the exact all-parameter bytecode and spill-free IR/register shape.
The compiler proof exhausts 768 profile/arithmetic/guard combinations at each
of the semantic and post-register-allocation gates and admits exactly twelve
total profiles. The runtime proof checks noncommutative FDIV operands, both
FCMP operand directions, exact ARM64/arm64e instruction words, strict and
inclusive equality boundaries, type exits, NaN, infinities, signed zero,
STOPREQ cleanup/reuse, and adjacent no-trace shapes. In particular, the proof
distinguishes ascending inclusive `+Inf` limits from strict exit behavior and
descending inclusive zero limits from strict underflow-to-zero behavior.

## Minimal-divergence cleanup

The cleanup restored the unchanged target options in `src/Makefile` exactly to
the `v2.1` version, removed a duplicate API declaration, kept ARM64-only
metadata and several lifecycle paths out of non-ARM builds, and normalized 167
DynASM lines to LuaJIT's existing indentation. It also corrected two cross-target
leaks: ARM64's exact five-slot side certificate and FORI/FORL tuple check no
longer run on x86-64.

Relative to `v2.1`, the cleanup initially reduced the `src/` diff from 56 files
with 13,891 insertions and 1,815 deletions to 55 files with 13,820 insertions
and 1,733 deletions. The completed division family now leaves it at 55 files
with 13,871 insertions and 1,733 deletions. Large ARM64 admission and lifecycle
blocks remain in common files; moving them into target-local modules is a later
structural migration because the source-certificate scripts parse their current
boundaries. Independent semantic and post-register-allocation gates were
deliberately not deduplicated.

## Verification at `2b2868f7`

- `tools/ci/arm64_jit_fail_closed_gate.sh`: passed in full.
- Native ARM64 vendored LuaJIT suite: `509 passed`.
- Focused all-parameter numeric contract: direct plus two randomized runs on
  ordinary ARM64 and arm64e/BTI; all six executions passed, exercising twelve
  profiles per process (72 profile executions total).
- Disposable thin x86_64 build of the identical production source at
  `6d94a92d`: platform smoke passed under Rosetta with
  `jit.os=OSX`, `jit.arch=x64`; its vendored suite also reported `509 passed`.
- The x86_64 canary published real first-level side traces and a live `JFORL`
  root, confirming that the ARM64-only widening did not alter x64 admission.
- Native `src/luajit`, `libluajit.a`, `lj_vm.o`, and `lj_asm.o` artifacts were
  thin ARM64, and the archive's `lj_asm.o` was byte-identical to the standalone
  object.

The descending runtime certificate confirms exact limit-first FCMP emission,
strict `HS`/`LO` and inclusive `HI`/`LS` branches, all six equality outcomes,
and the terminating/nonterminating IEEE cases for both new profiles.

The recurring unused `ccall_rawchild_wait` warning remains pre-existing. The
diagnostic GDB-JIT and x86_64 builds also emit the known unused `topofs`
warning; the x86_64 build's default-architecture clean probe reports its
expected non-GC64 rejection before the configured GC64 build succeeds.

## Still closed or incomplete

- General ARM64 IR admission, arbitrary Lua programs, and unrestricted spills
  or register layouts.
- Reversed or fixed division operands, extra division recurrences, dynamic-step
  numeric `FORL`, and general root geometries.
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

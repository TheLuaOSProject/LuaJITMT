# LuaJITMT ARM64 port status

This directory contains only the live status and durable design constraints.
The 938 append-only investigation and checkpoint notes were moved to
[`archive/`](archive/README.md) so historical observations are still available
without looking like current claims.

## Current checkpoint

Development branch: `codex/aarch64-macos-port`.

Last fully verified source checkpoint: `364449e6` (2026-08-28). It adds the
exact inclusive ascending `DIV_LE` root to the reviewed minimal-divergence
checkpoint without opening adjacent division shapes.

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
  `SUB_GE`, `MUL_LT`, `MUL_LE`, `DIV_LT`, and `DIV_LE` loop profiles; and
- callback-result lifetime across post-detach TG reclamation.

The two ascending division profiles are deliberately narrow:

```lua
while x < limit do
  x = x / divisor
end

while x <= limit do
  x = x / divisor
end
```

They require the exact all-parameter bytecode and spill-free IR/register shape.
The compiler proof exhausts 640 profile/arithmetic/guard combinations at each
of the semantic and post-register-allocation gates and admits exactly ten total
profiles. The runtime proof checks noncommutative FDIV operands, exact
ARM64/arm64e instruction words, strict and inclusive equality boundaries,
type exits, NaN, infinities, signed zero, STOPREQ cleanup/reuse, and adjacent
no-trace shapes. In particular, an inclusive `+Inf` limit is proved
nonterminating and interruptible while the strict profile exits.

## Minimal-divergence cleanup

The cleanup restored the unchanged target options in `src/Makefile` exactly to
the `v2.1` version, removed a duplicate API declaration, kept ARM64-only
metadata and several lifecycle paths out of non-ARM builds, and normalized 167
DynASM lines to LuaJIT's existing indentation. It also corrected two cross-target
leaks: ARM64's exact five-slot side certificate and FORI/FORL tuple check no
longer run on x86-64.

Relative to `v2.1`, the `src/` diff fell from 56 files with 13,891
insertions and 1,815 deletions to 55 files with 13,820 insertions and 1,733
deletions. Large ARM64 admission and lifecycle blocks remain in common files;
moving them into target-local modules is a later structural migration because
the source-certificate scripts parse their current boundaries. Independent
semantic and post-register-allocation gates were deliberately not deduplicated.

## Verification at `364449e6`

- `tools/ci/arm64_jit_fail_closed_gate.sh`: passed in full.
- Native ARM64 vendored LuaJIT suite: `509 passed`.
- Focused all-parameter numeric contract: direct plus two randomized runs on
  ordinary ARM64 and arm64e/BTI; all six executions passed, exercising ten
  profiles per process.
- Disposable thin x86_64 build: platform smoke passed under Rosetta with
  `jit.os=OSX`, `jit.arch=x64`; its vendored suite also reported `509 passed`.
- The x86_64 canary published real first-level side traces and a live `JFORL`
  root, confirming that the ARM64-only widening did not alter x64 admission.
- Native `src/luajit`, `libluajit.a`, `lj_vm.o`, and `lj_asm.o` artifacts were
  thin ARM64, and the archive's `lj_asm.o` was byte-identical to the standalone
  object.

The earlier `2fd6fb49` functional checkpoint also ran twelve independent native
processes which published exactly trace 1 with `link=1` and `linktype=loop`,
including genuine-NUM `MUL_LT`, `MUL_LE`, and `DIV_LT` operands.

The recurring unused `ccall_rawchild_wait` warning remains pre-existing. The
diagnostic GDB-JIT and x86_64 builds also emit the known unused `topofs`
warning; the x86_64 build's default-architecture clean probe reports its
expected non-GC64 rejection before the configured GC64 build succeeds.

## Still closed or incomplete

- General ARM64 IR admission, arbitrary Lua programs, and unrestricted spills
  or register layouts.
- Descending `DIV_GT`/`DIV_GE`, reversed or fixed division operands, and extra
  division recurrences. The descending pair is the next coherent DIV tranche.
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

# LuaJITMT ARM64 port status

This directory contains only the live status and durable design constraints.
The 938 append-only investigation and checkpoint notes were moved to
[`archive/`](archive/README.md) so historical observations are still available
without looking like current claims.

## Current checkpoint

Development branch: `codex/aarch64-macos-port`.

Last complete JIT fail-closed-gate checkpoint: `b940017e` (2026-08-29).
Current cleanup checkpoint: `c8cd7858` (2026-08-29). The intervening
`96ade477` keeps the ARM64 `JFORL` native entry out of JIT-disabled builds.
`c8cd7858` fixes the external-unwind final landing so ARM64 and arm64e preserve
`errno` after the system unwinder returns from the personality, matching the
existing x64 contract. The current checkpoint passed the complete native
bootstrap gate and focused JIT-unwind checks; the broader JIT fail-closed gate
has not been rerun since `b940017e`.

The exact ADD_LT invariant-INT-limit widening was introduced at `7d2a07bb`,
independently certified at the semantic and post-register-allocation gates by
`d7a2ec16`, and proved at runtime by `c85ac964`. The behavior-neutral cleanup
at `32fe50cc` restored LuaJIT-style switch layout, simplified one redundant
reference bound, made argument-kind rejection explicit, and renamed stale
local instruction macros. `20558576` bound that reject fallback into the
source certificate, and `b940017e` added the new proof to the umbrella gate.

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
- ARM64 and arm64e external-unwind C, fast-function, and JIT side-exit
  landings which restore OS error state after context installation without
  using locks or Apple's reserved `x18`;
- constant-step integer `FORL`, bounded integer and mixed numeric `LOOP`
  roots, fixed-half and dynamic-step numeric roots, literal-true `FUNCF`, and
  exact all-parameter `ADD_LT`, `ADD_LE`, `ADD_GT`, `ADD_GE`, `SUB_GT`,
  `SUB_GE`, `MUL_LT`, `MUL_LE`, `DIV_LT`, `DIV_LE`, `DIV_GT`, and `DIV_GE`
  loop profiles, each with either a NUM step or one invariant INT step widened
  once to NUM, plus the exact `ADD_LT` NUM-accumulator/NUM-step profile with one
  invariant INT limit widened once to NUM; and
- callback-result lifetime across post-detach TG reclamation.

The all-parameter profiles are deliberately narrow. For example, the four
division profiles are:

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
For INT-step traces, the only accepted conversion is the invariant slot-4 INT
step widened once by `CONV num.int` before the first recurrence and before the
loop. The raw INT step must remain in an unspilled GPR, the converted step in an
unspilled FPR, and the recurrence operands must use the converted value.
For the single INT-limit profile, the same constraints apply to the slot-3
limit, except its conversion occurs after the first recurrence and before the
loop comparison. NUM X with an INT step, INT X, other arithmetic/guards, and
every other INT-limit profile remain closed.

The compiler proof exhausts 2,304 profile/arithmetic/guard/argument-kind
combinations at each of the independent semantic and post-register-allocation
gates and admits exactly 25: twelve NUM-step profiles, twelve INT-step profiles,
and the one INT-limit profile. It rejects duplicate, relocated, unrelated, or
extra conversions; raw-INT recurrences or comparisons; snapshots, aliases,
spills, renames, and reversed noncommutative operands. The runtime proof checks
exact `SCVTF`, arithmetic, comparison, and branch instruction words on ARM64
and arm64e/BTI, strict and inclusive equality boundaries, live parameter
substitution, type exits, NaN, infinities, signed zero, STOPREQ cleanup/reuse,
and INT32 minimum, maximum, zero, `+1`, and `-1` behavior.

## Minimal-divergence cleanup

The cleanup restored the unchanged target options in `src/Makefile` exactly to
the `v2.1` version, removed a duplicate API declaration, kept ARM64-only
metadata and several lifecycle paths out of non-ARM builds, and normalized 167
DynASM lines to LuaJIT's existing indentation. It also corrected two cross-target
leaks: ARM64's exact five-slot side certificate and FORI/FORL tuple check no
longer run on x86-64.

Relative to local and remote `v2.1` at `a649f737`, the cleanup initially reduced
the `src/` diff from 56 files with 13,891 insertions and 1,815 deletions to 55
files with 13,820 insertions and 1,733 deletions. The current checkpoint is 57
files with 14,336 insertions and 1,737 deletions (net 12,599). Neither exact INT
widening capability changed `lj_asm_arm64.h`, `lj_emit_arm64.h`, or
`vm_arm64.dasc`; both reuse upstream's existing `asm_conv()`/`SCVTF` lowering.

The unwind fix keeps the historical exception prefix and x64 path unchanged.
Its ARM64 tail uses one exception-owned `x28` carrier and one target-local VM
landing, preserves the full allocatable GPR/FPR and status-register state, and
returns through the original signed target. The landing is omitted from
Windows and `LUAJIT_NO_UNWIND` ARM64 builds, so it adds no unused VM surface to
those configurations.

Large ARM64 admission and lifecycle blocks remain in common files. Moving them
into target-local modules is a later structural migration because current
source-certificate scripts parse their exact boundaries. Independent semantic
and post-register-allocation gates were deliberately not deduplicated: their
separation prevents a common-mode acceptance bug.

## Verification

- `tools/ci/arm64_jit_fail_closed_gate.sh`: passed in full at `b940017e`.
- Native ARM64 vendored LuaJIT suite during this cleanup tranche: `509 passed`.
- `tools/ci/arm64_bootstrap_gate.sh`: passed at `c8cd7858`, including 387
  vendored tests, TG/root/safepoint/protected-call contracts, threading and
  coroutine tests, and 320 FFI callback rounds across four threads.
- `tools/ci/arm64_oserr_unwind_contract.sh`: passed at `c8cd7858` for ordinary
  ARM64, arm64e/BTI, and the x86_64/Rosetta oracle. The injected unwinder
  clobber changed the unfixed ARM64 result from `EDOM` (33) to `EILSEQ` (92);
  every fixed landing observed 33.
- `tools/ci/arm64e_jit_unwind_contract.sh`: passed a registered synthetic JIT
  frame through the production personality, authenticated trampoline, and
  landing while preserving `errno` under the same clobber injection.
- A separate `LUAJIT_NO_UNWIND` ARM64 build passed and contained no
  `lj_vm_unwind_os_eh` symbol; the ordinary experimental build was restored.
- The compiler certificate exhaustively checked 2,304 candidates and admitted
  exactly 25 at each independent gate.
- The runtime certificate ran direct plus two randomized executions on ordinary
  ARM64 and arm64e/BTI: six processes total, 25 modes per process, and 150
  profile-mode executions.
- Disposable thin x86_64 build at `b940017e`: platform smoke passed under
  Rosetta with `jit.os=OSX`, `jit.arch=x64`, and JIT enabled; its vendored suite
  also reported `509 passed`.
- The x86_64 canary published real first-level side traces and a live `JFORL`
  root, confirming that the ARM64-only widening did not alter x64 admission.
- Native `src/luajit`, `libluajit.a`, `lj_vm.o`, and `lj_asm.o` artifacts were
  thin ARM64, and the archive's `lj_asm.o` was byte-identical to the standalone
  object.

The INT-step and INT-limit machine-code certificates each confirm exactly one
hoisted `SCVTF` at the shape-specific position, before its loop use, while
retaining the established limit-first FCMP direction and strict/inclusive
branch polarity. The new INT-limit trace is 140 bytes with an 80-byte loop
offset on ordinary ARM64 and 144 bytes with an 84-byte loop offset on
arm64e/BTI.

The recurring unused `ccall_rawchild_wait` warning remains pre-existing. The
diagnostic GDB-JIT and x86_64 builds also emit the known unused `topofs`
warning; the x86_64 build's default-architecture clean probe reports its
expected non-GC64 rejection before the configured GC64 build succeeds.

## Still closed or incomplete

- General ARM64 IR admission, arbitrary Lua programs, and unrestricted spills
  or register layouts.
- Recording-time INT accumulator conversion and general INT-limit conversion.
  Those shapes remain no-trace; only the invariant INT step profiles and the
  single exact `ADD_LT` invariant-INT-limit profile have widening certificates.
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

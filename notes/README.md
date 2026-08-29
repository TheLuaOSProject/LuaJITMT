# LuaJITMT ARM64 port status

This directory contains only the live status and durable architecture
constraints. The 938 historical investigation and checkpoint notes are under
[`archive/`](archive/README.md); they remain available without looking like
current claims.

## Current checkpoint

Development branch: `codex/aarch64-macos-port`.

Last complete JIT fail-closed-gate checkpoint: `e1eb3de1` (2026-08-29).
That structural checkpoint moved the ARM64 admission policy out of the shared
assembler without changing it. The native and x86_64 vendored suites last
passed with 509 tests each at `4d1b6126`; the temporary x86_64 build reported
`jit.os == "OSX"`, `jit.arch == "x64"`, enabled JIT, and a real loop trace
before it was removed.

The current cleanup and proof sequence is:

- `c8cd7858` preserves `errno` across ARM64 and arm64e external unwind;
- `218cafe2` admits one exact INT-accumulator widening grammar across the 12
  existing all-parameter numeric profiles;
- `3ad39871` completes the compiler certificate and adds the native runtime
  certificate;
- `28892903` binds the new proof into the umbrella gate; and
- `4d1b6126` makes JFUNCF disassembly checks robust to the same-address
  `FUNCV`/`IFUNCV` symbol aliases emitted by Apple's tools; and
- `e1eb3de1` moves the unchanged 3,244-line ARM64 admission and post-RA
  certificate into `src/lj_asm_arm64_admit.h`.

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
- lockless ARM64 and arm64e OS-error restoration plumbing for C,
  fast-function, and JIT side-exit landings; direct runtime proof currently
  covers the C landing on both targets and a synthetic arm64e JIT landing;
- constant-step integer `FORL`, bounded integer and mixed numeric `LOOP`
  roots, fixed-half and dynamic-step numeric roots, literal-true `FUNCF`, and
  the exact `ADD_LT`, `ADD_LE`, `ADD_GT`, `ADD_GE`, `SUB_GT`, `SUB_GE`,
  `MUL_LT`, `MUL_LE`, `DIV_LT`, `DIV_LE`, `DIV_GT`, and `DIV_GE`
  all-parameter loop profiles; and
- callback-result lifetime across post-detach TG reclamation.

## Exact numeric JIT boundary

The 12 all-parameter profiles accept three accumulator/step layouts:

- NUM accumulator and NUM step;
- NUM accumulator with one invariant INT step widened once before the first
  recurrence; and
- INT accumulator with a NUM step, widened before the first recurrence and
  exactness-checked through INT after `XPOLL` before the body recurrence.

One additional argument layout is accepted only for exact `ADD_LT`: a NUM
accumulator and NUM step with an invariant INT limit. The limit is widened
after the first recurrence and before the loop comparison. This yields 36
profile/layout modes plus the one INT-limit mode.

Every admitted trace has exact IR and snapshots plus spill-free register
classes and liveness relations; safe register remaps are allowed. For INT-step
traces, the raw slot-4 value stays in an unspilled GPR and its converted value
stays in an unspilled FPR. The natural allocation exercised by the
INT-accumulator runtime certificate keeps raw slot 2 in `x1`, the widened
accumulator in `d2`, and the post-`XPOLL` checked conversion in `x28`, followed
by `FCVTZS`, `SCVTF`, `FCMP`, and `BNE`. Fractional or out-of-INT32 values
leave through the shared `XPOLL` snapshot before the body recurrence.

The compiler certificate exhausts 3,072
profile/arithmetic/guard/argument-kind combinations at each independent
semantic and post-register-allocation gate. It admits exactly 37: twelve
NUM-step profiles, twelve INT-step profiles, twelve INT-accumulator profiles,
and the single INT-limit profile. Duplicate, relocated, unrelated, or extra
conversions; raw-INT recurrences or comparisons; snapshot mutations,
prohibited live-range aliases, spills, invalid renames, and reversed
noncommutative operands remain rejected.

The runtime certificate runs direct plus two randomized executions on ordinary
ARM64 and arm64e/BTI: six processes, 37 modes per process, and 222
profile-mode executions. It checks exact instruction words and offsets, live
parameter substitution, strict and inclusive equality, type exits, NaN,
infinities, signed zero, checked-conversion failures, STOPREQ cleanup/reuse,
flush/re-record, and the absence of side traces.

The INT-step and INT-limit shapes contain one hoisted `SCVTF`. The
INT-accumulator shape contains the initial `SCVTF` and the post-`XPOLL`
`FCVTZS`/`SCVTF` exactness check. Its trace is 156 bytes with an 80-byte loop
offset on ordinary ARM64 and 160 bytes with an 84-byte loop offset on
arm64e/BTI. The INT-limit trace remains 140/80 bytes on ARM64 and 144/84 bytes
on arm64e/BTI.

## Minimal-divergence cleanup

The cleanup restored unchanged target options in `src/Makefile` to the `v2.1`
form, removed a duplicate API declaration, kept ARM64-only metadata and
lifecycle paths out of non-ARM builds, normalized 167 DynASM lines to LuaJIT
indentation, restored LuaJIT-style switch layout, and removed redundant or
stale local names. ARM64's five-slot side certificate and FORI/FORL tuple
check no longer leak into x86_64 behavior.

Relative to local and remote `v2.1` at `a649f737`, the first cleanup reduced
the `src/` diff from 56 files with 13,891 insertions and 1,815 deletions to 55
files with 13,820 insertions and 1,733 deletions. The verified checkpoint is 58
files with 14,494 insertions and 1,737 deletions (net 12,757). The exact INT
widening tranches do not change `lj_asm_arm64.h`, `lj_emit_arm64.h`, or
`vm_arm64.dasc`; they reuse upstream's existing `asm_conv()` lowering.

The target-local extraction reduces the shared `src/lj_asm.c` divergence from
3,806 insertions and 5 deletions to 563 insertions and 5 deletions. Its moved
body was byte-for-byte compared with the pre-extraction source, and all source
certificates now inspect `src/lj_asm_arm64_admit.h` while retaining their
independent semantic and post-register-allocation boundaries.

The unwind fix keeps the historical exception prefix and x64 path unchanged.
Its ARM64 tail uses one exception-owned `x28` carrier and a target-local VM
landing, preserves the allocatable GPR/FPR state plus `NZCV`, `FPCR`, and
`FPSR`, and returns through the original target, return-signed on arm64e. The
landing is omitted from Windows and `LUAJIT_NO_UNWIND` ARM64 builds.

Large ARM64 lifecycle blocks still remain in common files, particularly
`src/lj_trace.c`. Move those only as separate structural changes with exact
source-boundary and runtime proof; do not combine or weaken the independent
semantic and post-register-allocation gates merely to reduce the diff.

## Verification

- `tools/ci/arm64_jit_fail_closed_gate.sh`: passed in full at `e1eb3de1`,
  including the 3,072-case compiler proof, 222 runtime profile executions,
  ARM64/arm64e publication, entry, exit, retirement, flush/reuse, GDB-JIT,
  callback, first-side, and safepoint contracts.
- Native ARM64 experimental build vendored suite: `509 passed` at
  `4d1b6126`.
- Disposable thin x86_64 build: platform smoke, real Rosetta loop trace, and
  vendored suite `509 passed` at `4d1b6126`.
- `tools/ci/arm64_bootstrap_gate.sh`: passed at `c8cd7858`, including 387
  vendored tests, TG/root/safepoint/protected-call contracts, threading and
  coroutine tests, and 320 FFI callback rounds across four threads.
- `tools/ci/arm64_oserr_unwind_contract.sh`: passed at `c8cd7858` for ordinary
  ARM64, arm64e/BTI, and the x86_64/Rosetta oracle. The injected unwinder
  clobber changed the unfixed ARM64 result from `EDOM` (33) to `EILSEQ` (92);
  every fixed landing observed 33.
- `tools/ci/arm64e_jit_unwind_contract.sh`: passed a registered synthetic JIT
  frame through the production personality and authenticated synthetic
  carrier/landing while preserving `errno`; it does not prove real side-exit
  resolution.
- A separate `LUAJIT_NO_UNWIND` ARM64 build passed and contained no
  `lj_vm_unwind_os_eh` symbol.
- `tools/ci/arm64_jit_root_entry_contract.sh`,
  `arm64_jit_funcf_record_contract.sh`, and
  `arm64e_jit_trace_pauth_contract.sh` all passed after the alias-boundary
  cleanup.

The recurring unused `ccall_rawchild_wait` warning remains pre-existing.
Diagnostic GDB-JIT and x86_64 builds also emit the known unused `topofs`
warning. The x86_64 builder's preliminary host-architecture clean probe emits
the expected non-GC64 rejection before the configured GC64 target succeeds.

## Still closed or incomplete

- General ARM64 IR admission, arbitrary Lua programs, and unrestricted spills
  or register layouts.
- General recording-time INT accumulator and INT-limit conversion. Only the
  exact 12-profile INT-accumulator grammar and single `ADD_LT` INT-limit
  grammar are open.
- Reversed or fixed division operands, extra division recurrences,
  dynamic-step numeric `FORL`, and general root geometries.
- General side traces, side-of-side traces, and stitches. Only explicitly
  certified first-side shapes are open.
- Uncertified conversions, calls, allocations, heap effects, table/upvalue JIT
  fast paths, and traced generic FFI calls.
- Direct OS-error-clobber proof for the fast-function landing and production
  JIT side-exit resolution on ARM64/arm64e. Current JIT unwind evidence uses a
  synthetic arm64e carrier and landing.
- Full parity with every x86_64 lockless VM, JIT, FFI, profiler, unwind, and
  stress path.
- The complete sanitizer, weak-memory, sustained-concurrency, and performance
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

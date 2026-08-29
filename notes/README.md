# LuaJITMT ARM64 port status

This directory contains only the live status and durable architecture
constraints. The 938 historical investigation and checkpoint notes are under
[`archive/`](archive/README.md); they remain available without looking like
current claims.

## Current checkpoint

Development branch: `codex/aarch64-macos-port`.

Last complete JIT fail-closed-gate checkpoint: `a7056fe5` (2026-08-29).
The admitted boundary remains the one exact Darwin ARM64 `CALLXS` root opened
at `5b6d9e0d`: a cached direct CDECL function
cdata with signature `int32_t(int32_t)` in the certified 16-bytecode integer
`FORL` root. Mutable cdata `__call` lookup now samples through enumerated TG
roots, guards the current base-metatable root and exact function identity, and
uses TG-local `tmptv`/`tmptv2` storage in generated code. Semantic IR,
snapshots, bytecode, signature, trace ownership, and post-register-allocation
state are independently certified. Checkpoint `23f2f870` added exact errno,
forced-exit, callback, STOPREQ, cleanup, and reuse evidence for that scalar
root. Checkpoint `a7056fe5` then narrowed six shared source files back to their
actual ARM64 or `CALLXS` capability boundaries without widening admission. The
complete ARM64/arm64e fail-closed umbrella passed at that cleanup checkpoint.
Focused checkpoint `c56b7382` subsequently proved depth-two generated callback
re-entry and callback-originated error unwind in the same admitted root on
ordinary ARM64 and arm64e/BTI, again without changing production admission.
The preceding `0ce4313b` checkpoint introduced the target-local ARM64 `XSAVE`
staging backend; the native and x86_64 vendored suites last passed with 509
tests each at `4d1b6126`.

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
  fast-function, and JIT side-exit landings; injected-clobber runtime proof
  covers the final C and fast-function landings on ARM64, arm64e/BTI, and
  x86_64/Rosetta. The standalone arbitrary ARM64e JIT-frame contract remains
  synthetic, while the exact scalar `CALLXS` STOPREQ path now reaches the real
  trace unwind and landing on ARM64 and arm64e/BTI;
- constant-step integer `FORL` plus one exact spill-free
  variable-stop/variable-step integer shape in both directions, bounded
  integer and mixed numeric `LOOP` roots, fixed-half and dynamic-step numeric
  roots, literal-true `FUNCF`, and the exact `ADD_LT`, `ADD_LE`, `ADD_GT`,
  `ADD_GE`, `SUB_GT`, `SUB_GE`, `MUL_LT`, `MUL_LE`, `DIV_LT`, `DIV_LE`,
  `DIV_GT`, and `DIV_GE` all-parameter loop profiles;
- one exact scalar Darwin ARM64 FFI call root with native-frame entry/leave,
  rooted pre-call errno preservation under a deliberately clobbering wait,
  callee errno through normal return, result-guard exit, forced epoch exit,
  one-level and depth-two callbacks, callback-error and post-call STOPREQ
  unwind, exact no-replay effects, cleanup and reuse, remote GC/flush
  retirement, mutable cdata `__call` replacement races, and arm64e/BTI
  execution; and
- callback-result lifetime across post-detach TG reclamation.

The ARM64 `XSAVE` backend is now live only for that exact scalar certificate.
`LJ_HASJIT_FFI_CALLXS` includes experimental Darwin ARM64, but the recorder and
assembler still reject every other ARM64 signature and call-site geometry.
This is not generic ARM64 FFI lowering.

## Exact numeric JIT boundary

The separate variable-step integer `FORL` shape requires a variable STOP and
STEP, one hoisted direction guard, one guarded `ADDOV(STEP, STOP)` kept live by
the sole scoped `IR_USE`, the existing two unchecked induction adds, exact
snapshots, and a spill-free post-RA liveness certificate. Runtime proof covers
positive and negative roots, same-sign STEP substitution, opposite-direction
and stop-plus-step-overflow exits, flush, and re-record. Constant-STOP dynamic
STEP and every NUM/FP dynamic-step `FORL` remain closed.

The 12 all-parameter `BC_LOOP` profiles accept three accumulator/step layouts:

- NUM accumulator and NUM step;
- NUM accumulator with one invariant INT step widened once before the first
  recurrence; and
- INT accumulator with a NUM step, widened before the first recurrence and
  exactness-checked through INT after `XPOLL` before the body recurrence.

One additional argument layout is accepted only for exact `ADD_LT`: a NUM
accumulator and NUM step with an invariant INT limit. The limit is widened
after the first recurrence and before the loop comparison. This yields 36
profile/layout modes plus the one INT-limit mode.

Every trace in this `BC_LOOP` family has exact IR and snapshots plus spill-free
register classes and liveness relations; safe register remaps are allowed. For INT-step
traces, the raw slot-4 value stays in an unspilled GPR and its converted value
stays in an unspilled FPR. The natural allocation exercised by the
INT-accumulator runtime certificate keeps raw slot 2 in `x1`, the widened
accumulator in `d2`, and the post-`XPOLL` checked conversion in `x28`, followed
by `FCVTZS`, `SCVTF`, `FCMP`, and `BNE`. Fractional or out-of-INT32 values
leave through the shared `XPOLL` snapshot before the body recurrence.

The all-parameter `BC_LOOP` compiler certificate exhausts 3,072
profile/arithmetic/guard/argument-kind combinations at each independent
semantic and post-register-allocation gate. It admits exactly 37: twelve
NUM-step profiles, twelve INT-step profiles, twelve INT-accumulator profiles,
and the single INT-limit profile. Duplicate, relocated, unrelated, or extra
conversions; raw-INT recurrences or comparisons; snapshot mutations,
prohibited live-range aliases, spills, invalid renames, and reversed
noncommutative operands remain rejected.

The all-parameter `BC_LOOP` runtime certificate runs direct plus two randomized
executions on ordinary ARM64 and arm64e/BTI: six processes, 37 modes per
process, and 222 profile-mode executions. It checks exact instruction words
and offsets, live parameter substitution, strict and inclusive equality, type
exits, NaN, infinities, signed zero, checked-conversion failures, STOPREQ cleanup/reuse,
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

Relative to local and remote `v2.1` at `a649f737`, the target-local extraction
keeps the large ARM64 policy body out of shared `src/lj_asm.c`. Its moved body
was byte-for-byte compared with the pre-extraction source, and source
certificates inspect `src/lj_asm_arm64_admit.h` while retaining independent
semantic and post-register-allocation passes. New numeric and CALLXS
certificates stay in that target-local header; shared recorder and trace edits
are limited to the runtime mechanisms they require.

The unwind fix keeps the historical exception prefix and x64 path unchanged.
Its ARM64 tail uses one exception-owned `x28` carrier and a target-local VM
landing, preserves the allocatable GPR/FPR state plus `NZCV`, `FPCR`, and
`FPSR`, and returns through the original target, return-signed on arm64e. The
landing is omitted from Windows and `LUAJIT_NO_UNWIND` ARM64 builds.

The `a7056fe5` source pass compared each touched shared file with upstream
`v2.1` at `a649f737`. It confines the external-unwind carrier to Darwin ARM64
external-unwind builds, confines `XSAVE` bodies and emitter helpers to the
`CALLXS` capability (or their explicit test helper), collapses a redundant
ARM64 recorder predicate, and restores the stock non-ARM trace-function
selection. These are preprocessor and target-boundary reductions; they do not
add another ABI, IR shape, signature, or runtime policy.

Large ARM64 lifecycle blocks still remain in common files, particularly
`src/lj_trace.c`. Move those only as separate structural changes with exact
source-boundary and runtime proof; do not combine or weaken the independent
semantic and post-register-allocation gates merely to reduce the diff.

## Verification

- `tools/ci/arm64_jit_fail_closed_gate.sh`: passed in full at `a7056fe5`,
  including the exact variable-step integer `FORL`, its overflow and direction
  exits, the 3,072-case `BC_LOOP` compiler proof, 222 runtime profile
  executions, ARM64/arm64e publication, entry, exit, retirement, flush/reuse,
  GDB-JIT, callback, first-side, and safepoint contracts.
- `tools/ci/arm64_jit_emitter_contract.sh`: passed at `a7056fe5`; the emitted
  owner-private `ffi_xsave_baseslot` and `ffi_xsave_nslots` publications are
  naturally sized `STLR w6` stores through x25, and `ffi_xsave_root` uses the
  existing pointer-sized release helper.
- `m7_ffi_callxs_arm64_scalar`: passed at `c56b7382`; the authentic trace and
  semantic/post-RA mutation fixture passed with exactly two `XSAVE` and two
  `CALLXS` nodes. Stable and concurrently raced cdata `__call` replacement,
  remote full GC, and remote `jit.flush()` completed without replay or stall.
  Its exact 16-bytecode lifecycle fixture also passed rooted-retry errno,
  result-guard and epoch exits, depth-two generated callback re-entry,
  callback-error and real STOPREQ unwind, cleanup, reuse, and exact
  foreign-effect/no-replay oracles.
- Manual arm64e/BTI build: the same semantic/post-RA fixture, production call,
  exact lifecycle, and threaded lifecycle passed through `a7056fe5` in thin
  ARM64e executables. The focused `c56b7382` lifecycle fixture additionally
  passed its nested-callback and callback-error cases as a thin ARM64e
  executable and dylib with explicit unwind tables.
- Disposable x86_64/Rosetta builds: the complete
  `m7_ffi_callxs_authentic` suite passed at `c05a7cd8`, including strict
  generated `CALLT`, `CALLMT`, `CALLM`, callback, forced-exit, STOPREQ, and
  threaded scalar, pointer, boolean, and sret remote-flush paths. Its three
  nested topology helpers deliberately resolve the identical published global
  library instead of a constant callee's captured upvalue. This preserves the
  intended bytecodes, frames, snapshots, calls, and effects while preventing
  an unrelated pre-`XSAVE` x64 upvalue guard from turning the lifecycle oracle
  into interpreter fallback. Trace selection also requires the exact runnable
  root and starting prototype rather than accepting the first retained
  `CALLXS` body.
- Disposable experimental ARM64 `LUAJIT_DISABLE_FFI` build: a native JIT loop
  published and executed while `require("ffi")` remained unavailable at
  `a7056fe5`.
- `tools/ci/arm64_jit_first_side_production_contract.sh`: passed at
  `a7056fe5` after the capability-boundary cleanup.
- Native ARM64 experimental build vendored suite: `509 passed` at
  `4d1b6126`.
- Disposable thin x86_64 build: platform smoke, real Rosetta loop trace, and
  vendored suite `509 passed` at `4d1b6126`.
- `tools/ci/arm64_bootstrap_gate.sh`: passed at `c8cd7858`, including 387
  vendored tests, TG/root/safepoint/protected-call contracts, threading and
  coroutine tests, and 320 FFI callback rounds across four threads.
- `tools/ci/arm64_oserr_unwind_contract.sh`: passed at `f06e79d5` for ordinary
  ARM64, arm64e/BTI, and the x86_64/Rosetta oracle. Both direct C and repeated
  fast-function final landings preserved `EDOM` (33) across an injected
  unwinder clobber to `EILSEQ` (92).
- `tools/ci/arm64e_jit_unwind_contract.sh`: passed a registered synthetic JIT
  frame through the production personality and authenticated synthetic
  carrier/landing while preserving `errno`; it does not prove real side-exit
  resolution.
- A separate `LUAJIT_NO_UNWIND` ARM64 build passed and contained no
  `lj_vm_unwind_os_eh` symbol.
- `tools/ci/arm64_jit_root_entry_contract.sh`,
  `arm64_jit_funcf_record_contract.sh`, and
  `arm64e_jit_trace_pauth_contract.sh` all passed after the alias-boundary
  cleanup. The root-entry source-order probe was updated for the owner-aware
  admission signature and now runs without empty-counter arithmetic warnings.

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
- Reversed or fixed division operands, extra division recurrences, NUM/FP
  dynamic-step `FORL`, every integer dynamic-step geometry except the exact
  variable-STOP/variable-STEP shape, and general root geometries.
- General side traces, side-of-side traces, and stitches. Only explicitly
  certified first-side shapes are open.
- Uncertified conversions, allocations, heap effects, table/upvalue JIT fast
  paths, and every ARM64 FFI call except the exact direct
  `int32_t(int32_t)` CDECL root. Other signatures and call-site geometries
  remain closed.
- General production JIT side-exit unwind resolution on ARM64/arm64e. The
  exact scalar `CALLXS` STOPREQ trace unwind is real; the standalone arbitrary
  arm64e JIT-frame carrier/landing evidence remains synthetic.
- General x86_64 re-entry through a constant-inlined closure's captured-upvalue
  guard. The strict `CALLXS` lifecycle fixture isolates this unrelated path by
  resolving its already-published library global; its generated `CALLT`,
  `CALLMT`, and `CALLM` paths now pass.
- Full parity with every x86_64 lockless VM, JIT, FFI, profiler, unwind, and
  stress path.
- The complete sanitizer, weak-memory, sustained-concurrency, and performance
  recovery matrix from the original roadmap.

Do not describe this branch as a completed ARM64 port yet. A passing stock
suite proves an important compatibility boundary, not general JIT coverage.

The admitted scalar lifecycle now has nested-callback and callback-error unwind
evidence. Certify `double(double)` as the next exact signature and continue one
signature at a time: pointer, 64-bit integer, boolean, aggregate/sret, and
variadic. Indirect function pointers and general call-site/root layouts remain
separate boundaries. Before admitting general table/helper traces, ARM64
`lj_vm_next` also needs the forwarding-safe traversal already used by the
x86_64 path.

## Primary checks

```sh
env MACOSX_DEPLOYMENT_TARGET=13.0 tools/ci/arm64_jit_fail_closed_gate.sh
env MACOSX_DEPLOYMENT_TARGET=13.0 LUA_PATH='tests/lib/?.lua;src/?.lua;src/jit/?.lua;;' \
  src/luajit tools/test.lua m7_ffi_callxs_arm64_scalar
env LUA=src/luajit tools/ci/lua_test.sh run_stock_tests -- --quiet
```

Use focused contracts while developing and run the complete gate before a
checkpoint claim. Keep coherent source, compiler-certificate, runtime-proof,
and documentation changes in separate commits and push each validated slice.

See [`architecture-constraints.md`](architecture-constraints.md) before
changing the VM, JIT, FFI, GC2, publication, or cleanup paths.

# ARM64 synthetic native-exit checkpoint (2026-08-26)

This isolated tranche is based exactly on `61d2a639`. It makes the existing
ARM64 native trace-exit substrate TG-local and testable without enabling the
recorder or native entry. `LJ_ARM64_JIT_FAIL_CLOSED` remains set, hot numeric
loops still publish zero traces, and `IR_XPOLL` is deliberately outside this
change.

## Implemented contract

- `asm_exitstub_setup()` authenticates an arm64e C function pointer before
  direct-branch range arithmetic. Both tail sizing and final tail emission do
  the same for `lj_vm_exit_interp`, so reservation and fixup derive their
  branch-range decisions from one normalized code address. Indirect
  handler/interpreter pointers retain the existing signed-k64 plus
  `BLRAAZ`/`BRAAZ` contract.
- The exit-stub writer is shared with a test-only wrapper. Direct stubs remain
  `STR LR; BL handler; MOVZ parent; per-exit BL`, and indirect stubs remain
  `STR LR; LDR signed handler; BLRAAZ/BLR; MOVZ parent; per-exit BL`.
  `exitstub_trace_addr_()` and the handler formula
  `(saved_lr-handler_lr)/4-2` are checked for every synthetic exit.
- ARM64 `ExitState` now statically binds C to the VM's 512-byte save prefix:
  32 eight-byte FPRs, 32 pointer-width GPRs, GPR offset 256, spill offset 512.
- `vm_exit_handler` acquire-loads both `cur_L` and `jit_base` relative to fixed
  x25. It publishes EXIT but does not clear `jit_base` before
  `lj_trace_exit()`. Thus the TG lease spans TraceVec/body lookup, protected
  snapshot restoration, FFI cleanup, SMR leave and TEXIT delivery.
- `lj_trace_exit()` captures the physical executing TG with `G2TG(G(L))` on
  ARM64. Error unwind transport and the gcdefer early-quiescence case use that
  exact TG's `jit_exitcode` and `jit_base`; they no longer use shared
  `J->exitcode` or a migratable `L2TG(L)` approximation.
- Both normal and error `vm_exit_interp` paths publish `L->base`, release-clear
  TG `jit_base`, release-publish INTERP, separately acquire-load 32-bit `poll`
  and `profile_request`, and call `lj_safepoint_ack_check()` when either is
  pending. The result/error and PC are saved in the C frame; L and BASE are
  reloaded after the call because stack service may relocate them.
- The shared global vmstate store remains a compatibility mirror. The native
  exit correctness path reads TG `cur_L`, `jit_base`, `jit_exitcode` and
  vmstate publications; it does not read a global executor-state authority.

Current native disassembly resolves the relevant TG offsets as:

- `poll`: x25 + `0x808`, `LDAR w8`;
- `profile_request`: x25 + `0x80c`, `LDAR w9`;
- `cur_L`: x25 + `0x820`, `LDAR x23`;
- `jit_base`: x25 + `0x830`, `LDAR x19` in the handler and `STLR xzr` in each
  interpreter completion path;
- `vmstate`: x25 + `0x84c`, 32-bit release publication.

The arm64e/BTI artifact contained `BTI c` at `lj_vm_exit_handler`, `BTI j` at
`lj_vm_exit_interp`, `BLRAAZ x30` in the synthetic indirect stub, and PAUTH
authentication instructions in `lj_asm.o`.

## Synthetic evidence

`tests/t-arm64-jit-exit.c` invokes the production stub writer through a
test-only surface and validates direct and indirect layouts, parent encoding,
all exit-slot addresses, LR-derived exit numbers, k64 addressing and the
ExitState offsets. It then runs 128 deterministic alternating normal/error
lease cycles against a concurrent idle-reclaimer:

1. TG `jit_base` is release-published and vmstate becomes EXIT.
2. The reclaimer must reject while the lease is non-null.
3. The error rounds publish/acquire/clear TG `jit_exitcode`.
4. The owner clears the lease, then publishes INTERP.
5. The reclaimer must enter successfully after quiescence.

The fixture also publishes a standalone profile request only after quiescence,
calls the checked owner acknowledgement, and verifies that the request is
consumed without recreating a JIT lease.

`tools/ci/arm64_jit_exit_contract.sh` performs native and arm64e builds, runs
the fixture in both modes, turns its emitted words into Mach-O text for real
disassembly, inspects the linked VM object, checks physical-TG C routing, runs
a hot-loop zero-trace smoke, and restores ordinary native experimental
artifacts. `tools/ci/arm64_vm_safepoint_contract.sh` now recognizes five source
call sites in a JIT-capable VM and three emitted calls in a compile-time no-JIT
VM.

## Validation results

- `tools/ci/arm64_jit_exit_contract.sh`: passed native ARM64 and arm64e/BTI;
  both fixture runs completed 128 normal/error races and the hot-loop smoke
  retained zero traces.
- `tools/ci/arm64_jit_fail_closed_gate.sh`: passed; JIT code/API remained
  linked, the existing emitter and interpreter safepoint contracts passed, and
  hot loops retained zero traces.
- `tools/ci/arm64_bootstrap_gate.sh`: passed in compile-time no-JIT mode,
  including the full source/object safepoint contract, `387 passed`, threading,
  hook/coroutine, C API, tmpbuf, signal/profile and FFI callback gates.
- Thin x86_64 assertion build with `TARGET_FLAGS=-arch x86_64`: compiled and
  linked; enabled-JIT numeric smoke produced a trace; the vendored stock suite
  reported `509 passed` under Rosetta.
- `git diff --check`: passed.

The build's recurring clean-time architecture probe prints a discarded
`lockless runtime requires GC64` diagnostic before the real configured build;
all listed builds subsequently completed successfully. Existing compiler
warnings in `lj_trace.c`, `lj_ccall.c`, and no-JIT `lj_func.c` were unchanged by
this tranche.

## Honest limitations and remaining blockers

The recorder and native entry are still fail-closed, so this checkpoint cannot
execute a real generated guard exit. In particular, it does not yet prove:

- end-to-end `lj_snap_restore_exit()` against a real live or retired trace body;
- a real throw crossing mcode and installing the unwind-selected stub PC;
- stack-check exit rewriting or side-trace snapshot restoration in machine
  code;
- a STOPREQ/FLUSHJ/profile request arriving during actual snapshot restore;
- PAUTH/BTI behavior of a successfully entered generated trace;
- any ARM64 `IR_XPOLL` recording or lowering.

The lifetime race proves the exact TG/reclaimer primitive and the object gate
proves the VM ordering, but it is not a substitute for executing that assembly
with a real `ExitState`. Assertions still guard an invalid/missing trace body;
this tranche does not add a production fail-stop recovery for corrupted parent
or snapshot metadata.

The separately owned XPOLL tranche landed and was validated as `b92f6d9e`.
With this exit checkpoint integrated, native entry must still be enabled only
with the existing strict IR allowlist. The first enabled-root validation must
add real normal, guarded, error/unwind, stack-check, poll/profile, concurrent
flush/retirement and arm64e exit runs.

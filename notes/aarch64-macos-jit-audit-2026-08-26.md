# macOS ARM64 JIT audit and fail-closed scaffolding checkpoint

Date: 2026-08-26

## Outcome

The first checkpoint admits the JIT subsystem only for an explicitly requested
desktop macOS ARM64 experimental build, while keeping trace recording,
publication and native entry unreachable. It is a compile/link and ABI
scaffolding checkpoint, not an ARM64 JIT enablement claim.

The opt-ins now have separate contracts:

- `LUAJIT_MT_ARM64_BOOTSTRAP` remains the interpreter-only bootstrap and still
  requires `LUAJIT_DISABLE_JIT`.
- `LUAJIT_MT_ARM64_JIT_EXPERIMENTAL`, together with the bootstrap macro, admits
  `LJ_HASJIT=1` and derives `LJ_ARM64_JIT_FAIL_CLOSED=1`.
- The experimental macro is rejected without the bootstrap macro and is
  rejected when combined with `LUAJIT_DISABLE_JIT`.

`lj_trace_hot()` resets the hot counter and returns before taking the JIT token
or entering `LJ_TRACE_START`. Consequently no ARM64 trace can be recorded,
assembled, published, linked or entered in this checkpoint. `jit.status()` is
true and the JIT libraries are present, but `jit.util.traceinfo(1)` remains nil
under forced-hot numeric loops. `lj_trace_ins()` independently aborts any
unexpected token-owned recorder ingress, `trace_hotside()` returns before token
acquisition, and `lj_trace_stitch()` aborts unexpected owner state and returns.
These defensive gates prevent a direct or stale dispatch path from bypassing
the primary hot-loop stop.

## Implemented compile and API inventory

### Admission and fail-closed boundary

- `src/lj_arch.h`: defines the separate experimental admission macro contract
  and the always-defined derived `LJ_ARM64_JIT_FAIL_CLOSED` predicate.
- `src/lj_trace.c`, `lj_trace_hot`: performs the hard stop before recorder
  ownership. This is the primary safety boundary; the downstream ARM64
  fail-closed branches are defense in depth and stale-bytecode recovery.

### Explicit call contracts

The following formerly x64-only explicit contracts now also cover ARM64:

- `src/lj_trace.h`, `src/lj_trace.c`, `src/lj_dispatch.c`:
  `lj_trace_hot(J, pc, L)`.
- `src/lj_trace.h`, `src/lj_dispatch.h`, `src/lj_dispatch.c`:
  `lj_trace_stitch(J, pc, L, traceno)` and
  `lj_dispatch_stitch(J, pc, L, traceno)`.
- `src/lj_trace.h`, `src/lj_trace.c`, `src/vm_arm64.dasc`:
  `lj_trace_exit(J, exptr, L, parent, exitno)`.
- `src/lj_snap.h`, `src/lj_snap.c`, `src/lj_trace.c`:
  `lj_snap_restore_exit(J, exptr, L, T, parent, exitno)`.

These declarations remove the ARM64 dependency on the recorder-owned
`J->L/J->parent/J->exitno` fields. Native exit remains unreachable until the VM
exit handler is converted to the TG-local runtime protocol described below.

### ARM64 VM compile repairs

`src/vm_arm64.dasc` contains the concrete VM repairs:

- The three addresses formerly materialized as one out-of-range
  `add ..., #GG_G2J` immediate are split into `GG_G2J_HI` and `GG_G2J_LO` in
  `vm_hotloop`, `cont_stitch`, and `vm_exit_handler`.
- `vm_hotloop` passes `L` explicitly.
- The fail-closed `cont_stitch` cannot consult a raw `GCtrace *` or jump to a
  linked trace. It falls back to interpreter continuation.
- `vm_exit_handler` supplies explicit `L`, parent trace number and exit number.
  This only fixes the C ABI; the handler is intentionally unreachable here.
- The static-exit BC_JLOOP redispatch calls
  `lj_trace_stale_startins(J, pc, traceno, L)`, publishes `BASE` before the
  helper, reloads it afterward, and reloads the bytecode/traceno on every retry.
- Failed `BC_ISNEXT` despecialization no longer indexes `J->trace`. It uses
  `lj_trace_invalidate_itern`, exact bytecode CAS publication, and publishes
  `BC_JMP` only after the target is observably `BC_ITERC`.
- `BC_JLOOP` never reads a raw trace slot or mcode pointer. It uses
  `lj_trace_stale_startins` and statically redispatches the recovered original
  bytecode. Every retry freshly loads the bytecode and trace number.

There is no `GL_J(trace)` access left in the ARM64 VM source.

### Shared compiler repairs

- `src/lj_record.c`, `rec_func_xpoll`: ARM64 does not record `IR_XPOLL` until
  ARM64 lowering and memory-ordering semantics exist.
- `src/lj_record.c`, `rec_bufstr_is_tg_tmpbuf`: the x86-only predicate is now a
  preprocessor branch rather than an undefined composite macro used as a C
  expression.
- `src/lj_trace.c`, `lj_trace_initstate`: `LJ_K64_M2P64` initialization is
  limited to targets that define the slot.
- `src/lj_asm_arm64.h`, `asm_obar`: ARM64 `IR_OBAR` deterministically raises
  `LJ_TRERR_NYIIR`; it no longer references the removed
  `IRCALL_lj_gc_barrieruv` contract.
- `src/lj_emit_arm64.h`: the shared assembler's TG get/set and VM-state emitter
  contracts exist only as fail-closed errors. They emit no global-state
  approximation and therefore cannot silently become unsafe generated code.
- `src/lj_trace.c`, `trace_exittab_reset` and `trace_flushside`: ARM64 builds use
  the architecture's per-trace `exitstub_trace_addr()` contract rather than the
  grouped x64 `exitstub_addr()` contract.

### macOS executable-memory plumbing

`src/lj_mcode.c` enables the existing `LJ_MCODE_MAPJIT` path on desktop ARM64
as well as x64. iOS remains excluded. The existing path already owns the
`MAP_JIT` mapping flag, `pthread_jit_write_protect_np()` transitions and
`lj_mcode_sync()` instruction-cache invalidation. This checkpoint proves those
paths compile; it does not yet publish or execute ARM64 mcode.

## Current runtime gap inventory

The following work is required before removing the fail-closed return.

### 1. TG-local ARM64 emitter state

Files/functions:

- `src/lj_target_arm64.h`: retain `RID_DISPATCH = x25` in `RSET_FIXED`.
- `src/lj_emit_arm64.h`: replace the fail-closed `emit_gettg`, `emit_settg`,
  `emit_setvmstate`, and `emit_setvmstate_root` contracts with real x25-relative
  loads/stores to `TGState.cur_L`, `TGState.jit_base`, and `TGState.vmstate`.
- `src/lj_asm.c`: audit all shared call sites, especially `asm_head_root`,
  `asm_head_side`, BASE rematerialization, snapshot restore, and tail linking.
- `src/lj_asm_arm64.h`: remove remaining assumptions that `RID_GL` is the owner
  of per-executor state.

ARM64-specific instructions must use acquire/release operations where another
TG or GC2 observes the field. Ordinary `LDR/STR` is insufficient for
publication fields. x25 is already the fixed interpreter TG dispatch carrier,
so allocating another fixed register is unnecessary.

### 2. Exact JLOOP/JFUNC entry protocol

Files/functions:

- `src/vm_arm64.dasc`: `BC_JLOOP`, JIT function-header entry, direct links and
  stitch entry.
- `src/vm_x64.dasc`: use the current x64 entry sequence as the semantic model,
  not as an instruction-for-instruction port.
- `src/lj_jit.h`: reuse `traceref_safe`, `trace_runnable_acq`, trace identity,
  `startpc`, retirement/entry-gate, and mcode acquire helpers.

The ARM64 entry sequence must publish TG `jit_base`, order that publication
against the GC2 gate recheck, validate the exact TraceVec slot generation,
reject retired or entry-gated bodies, validate `startpc`, acquire a non-null
mcode pointer, and only then branch. Every rejection clears TG `jit_base` and
recovers/redispatches bytecode without waiting on a peer. JFUNC and stitched
links need the same runnable-retention proof; merely fixing BC_JLOOP is not
enough.

### 3. Exit and stitch conversion

Files/functions:

- `src/vm_arm64.dasc`: `vm_exit_handler`, `vm_exit_interp`, `cont_stitch` and
  the per-trace exit-stub sequence.
- `src/lj_trace.c`: `lj_trace_exit`, `trace_exit_cp`, `trace_hotside`, stale
  JLOOP return handling and trace-exit publication.
- `src/lj_snap.c`: `lj_snap_restore_exit` is now available; retain the exact
  `L/T/parent/exitno` path.

The VM must stop using raw global `jit_base` and recorder fields as an exit
mailbox. It must retain the exact parent body while snapshot metadata is read,
clear TG `jit_base` at the defined interpreter boundary, and preserve the
explicit IDs across safepoint/error paths. `cont_stitch` must resolve a
TraceVec slot by number and validate runnable retention rather than carrying a
raw trace pointer in a continuation frame.

### 4. Weakly ordered `IR_XPOLL`

Files/functions:

- `src/lj_record.c`: enable terminal and deep-FUNCF XPOLL recording for ARM64
  only after lowering exists.
- `src/lj_opt_loop.c`: generalize loop XPOLL policy from x64.
- `src/lj_asm.c` and `src/lj_asm_arm64.h`: add `IR_XPOLL` lowering.
- `src/lj_tg.h`: use the established TG poll/profile access contracts.

ARM64 lowering must acquire-load the TG poll/profile request state, branch to a
snapshot-bearing exit when nonzero, and prevent memory reordering across the
poll/exit boundary. A plain relaxed load would allow an apparently acknowledged
safepoint without the required stack/root publication visibility.

### 5. Safe initial IR allowlist

The first executable trace should be limited to numeric scalar loop IR plus
loads/stores whose MT and snapshot contracts are already proven. Until each
family is audited, reject it with `LJ_TRERR_NYIIR` or route through an existing
rooted C helper:

- table reads/writes, table allocation/resizing and barriers;
- upvalue/cell access and object barriers (`IR_OBAR` is already rejected);
- GC allocation, sinking and materialization;
- string/buffer allocation and TG temporary-buffer state;
- FFI calls, callbacks, cdata allocation and foreign unwind;
- stitched fast functions and side traces.

Do not accept an IR merely because the stock ARM64 assembler has an encoding.
The fork's TG-local roots, GC2 publication and retirement contracts are the
admission criteria.

### 6. Publication, linking and retirement

Files/functions:

- `src/lj_trace.c`: `trace_save`, slot publication, `trace_flushside`, scoped
  flush, retirement and reclaim.
- `src/lj_asm.c` plus `src/lj_asm_arm64.h`: root/side linking and exit targets.
- `src/lj_mcode.c`: write-mode ownership, cache sync, RX transition and reclaim.

The C publication/retirement machinery is already TraceVec-based and should be
generalized, not duplicated. ARM-specific work is needed wherever generated
branches or embedded per-trace exit stubs bypass the C slot lookup. Publication
must make metadata and synchronized mcode visible before the bytecode patch;
retirement must close entry/link gates before freeing metadata or mcode and must
honor native pins/SMR grace periods.

## Reuse versus ARM64-specific implementation

Generalize/reuse these existing x64 contracts:

- explicit `L/parent/exitno` hot, stitch, exit and snapshot APIs;
- `lj_trace_stale_startins` and `lj_trace_invalidate_itern`;
- TraceVec safe lookup, trace identity, runnable/entry-gate and retirement
  helpers in `lj_jit.h`;
- TG-local `cur_L`, `jit_base`, `vmstate`, poll and exit-code accessors;
- GC2 gate-close/recheck semantics, native body pins and SMR retirement;
- the ordering of metadata/mcode publication before bytecode publication.

Implement these specifically for ARM64:

- x25-relative TG load/store instruction sequences and acquire/release forms;
- weak-order entry fences and XPOLL lowering;
- pointer-authenticated mcode branches where enabled;
- per-trace ARM64 exit-stub address/patch encoding;
- Darwin ARM64 MAP_JIT write-protect integration and instruction-cache proof.

## Minimal ordered implementation tranche for the first native trace

1. Implement and unit-contract the x25 TG emitter operations and VM-state
   publication. Keep `lj_trace_hot` fail-closed.
2. Implement BC_JLOOP and JFUNC exact TraceVec entry rejection/acceptance,
   including TG `jit_base` publication and gate recheck. Exercise only synthetic
   rejected entries; keep the recorder fail-closed.
3. Complete exact ARM64 exit-stub decoding, explicit exit ABI, TG state clearing
   and snapshot restore. Exercise a synthetic exit against retained metadata;
   keep normal native entry disabled.
4. Lower `IR_XPOLL` with acquire semantics and a snapshot exit. Add a generated
   code inspection test plus a two-TG STOPREQ/FLUSHJ runtime test.
5. Add an explicit ARM64 IR allowlist for a pure numeric FORL/LOOP trace. All
   allocation, table, upvalue, buffer, FFI, stitch and side-trace IR remains NYI.
6. Enable root assembly and MAP_JIT publication for that allowlist, flush the
   instruction cache, transition to executable mode, then publish the TraceVec
   body and exact bytecode patch.
7. Remove the `lj_trace_hot` hard stop only for this allowlisted root case. Keep
   side recording and stitching disabled. Run the first trace, force a poll
   exit, flush it, cross an SMR grace period, and verify static redispatch.

The first implementation checkpoint is complete only when a native numeric
root trace executes and all of the following hold:

- exact trace count transitions `0 -> 1 -> 0` across publish and flush;
- no raw `J->trace`/global `jit_base` VM access exists;
- concurrent gate close either observes TG entry intent or makes entry reject;
- STOPREQ and FLUSHJ exit a long-running trace within the bounded runtime gate;
- snapshot restore uses the explicit `L/T/parent/exitno` body;
- MAP_JIT write protection is enabled during execution and cache sync covers the
  exact emitted range;
- stale JLOOP and failed ISNEXT recover without use-after-free or peer waits;
- prohibited IR families abort before mcode publication;
- native pins, TraceVec slots, bytecode and mcode are retired in the required
  order and reclamation waits for the grace boundary.

## Current checkpoint validation

`tools/ci/arm64_jit_fail_closed_gate.sh` is the clean native gate. It:

1. clean-builds and links a thin ARM64 assert build with both opt-ins;
2. proves the archive contains `lj_trace_hot`/`lj_trace_ins`;
3. proves the ARM64 mcode object references the Darwin JIT write-protect API;
4. rejects any raw `GL_J(trace)` reference in `vm_arm64.dasc`;
5. forces hot numeric loops and GC steps while asserting trace 1 is absent;
6. runs the ARM64 VM safepoint source contract (the disabled-JIT object's
   bytecode-symbol bounds intentionally differ in a JIT-enabled VM); and
7. runs `t-vm-safepoint.c` through the experimental interpreter-only branch,
   including remote STOPREQ of a non-terminating numeric loop.

The final quiescent-tree run of
`tools/ci/arm64_jit_fail_closed_gate.sh` passed. It completed the clean native
compile/link, found the recorder and Darwin JIT write-protect symbols, loaded
`jit.util`, reported JIT enabled, completed forced-hot numeric/GC loops with a
zero trace count, passed the safepoint source contract, and passed the focused
runtime fixture including remote STOPREQ of a numeric interpreter loop.

The separate interpreter-only regression gate also passed after placing every
`GG_G2J`-derived ARM64 DynASM macro and immediate assertion under `LJ_HASJIT`:
the clean `LUAJIT_MT_ARM64_BOOTSTRAP + LUAJIT_DISABLE_JIT` assert/root-helper
build, focused C-API suite, and ordinary-bootstrap restore were green. Thus the
experimental admission did not widen or break the established bootstrap
contract.

The widened x64 predicates retain their original x64 branches: x64 remains on
the existing explicit hot/stitch/exit/snapshot ABI, XPOLL recording/lowering,
grouped exit stubs and MAP_JIT path. Parent integration subsequently
cross-built `ee5ffe9f` as thin x86_64 with `TARGET_FLAGS=-arch x86_64`; its
enabled-JIT numeric smoke passed and the stock suite reported 509 passes under
Rosetta. `t-threading-api.lua -joff` panicked at its caught closed-channel error
on both `ee5ffe9f` and parent `cc420988`, confirming that failure was
pre-existing rather than a scaffolding regression.

## Hard blockers

There is no compiler or Darwin API blocker to the scaffolding checkpoint. The
hard correctness blockers to executing even one ARM64 trace are the missing
TG-relative emitter state, weak-order XPOLL, exact entry/exit retention, and a
strict safe-IR admission policy. MAP_JIT availability alone is not a readiness
signal. iOS remains out of scope because executable-memory policy differs and
the experimental macro explicitly requires desktop macOS.

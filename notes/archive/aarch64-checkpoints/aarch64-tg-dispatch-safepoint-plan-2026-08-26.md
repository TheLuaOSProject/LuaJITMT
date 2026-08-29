# Apple ARM64 TG dispatch and safepoint plan (2026-08-26)

## Why this is the next gate

The experimental interpreter bootstrap executes through the stock ARM64
universe-global dispatch machinery.  That is sufficient to validate assembler
compatibility, coroutine ownership, and the scalar FFI callback ABI, but it is
not a lockless VM: different OS threads overwrite shared `cur_L`, `vmstate`,
hook counters, and dispatch state.

The existing `tests/t-vm-safepoint.c` gives a positive red baseline on the
native ARM bootstrap.  Its first interpreted loop returns normally, but the
following `assert_acked()` aborts because `hs_pending` is still one.  The ARM
VM never consumed the TG poll request.  Separately,
`tests/t-threading-hooks.lua` passed four repeated runs and failed the fifth
because one live worker never redispatched to the new hook table.

Safepoint polling cannot simply be enabled first.  A root-scan acknowledgement
would be unsound while ordinary ARM stack writes and frame moves do not
invalidate `TG.stack_dirty_epoch`.  The required order is:

1. TG-local dispatch and owner-state publication;
2. complete stack-dirty and result-root publication;
3. VM safepoint acknowledgement and progress polls;
4. JIT trace/XPOLL work only after the interpreter gate is green.

## Stable ARM register ABI

Keep `x22` as `GL`.  It is the current interpreter global-state register,
`RID_GL`, and a fixed input throughout the ARM64 JIT backend.  Repurposing it
would mix the interpreter port with an unnecessary trace ABI rewrite.

Dedicate callee-saved `x25` to `DISPATCH`, pointing at `TGState.dispatch`.
`x25` currently caches `TISNUMhi`, but that value is used at only six type
checks.  Those checks can materialize the constant in a scratch register for
one extra instruction.  `x26` remains the much more heavily used
`TISNIL`/`ST_INTERP` constant.

`src/lj_target_arm64.h` must define `RID_DISPATCH = RID_X25` and include it in
`RSET_FIXED`, even while the bootstrap forces the JIT off.  This reserves the
future trace ABI now instead of letting generated traces allocate over the
interpreter's TG authority.

Useful native-host offsets relative to `TGState.dispatch` are currently:

- `poll`: 2056
- `profile_request`: 2060
- `cur_L`: 2080
- `vmstate`: 2124
- `hookmask_th`: 2221
- `stack_dirty_epoch`: 27920

The first five fit a single add/immediate addressing plan.  The dirty epoch
needs split addressing or a C helper.  Static assertions and an ARM object
contract should protect the assumed forms.

## Stage 1: TG dispatch and publication

Add ARM equivalents of the established x86 contracts:

- `load_DISPATCH`: acquire the running state's `tg_hint` and add
  `offsetof(TGState, dispatch)`;
- `load_G`: derive/revalidate `GL` from the dispatch-relative TG field;
- dynamic `ins_NEXT`/`ins_callt` and static redispatch: index from `x25`, not
  `GL + GG_G2DISP`;
- hot counters: use `DISPATCH + TG_DISP2HOT`;
- `cur_L`: release-store the running state to the TG field;
- `vmstate`: release-store the authoritative TG word and retain the global
  word only as a transitional mirror;
- hook mask: acquire-load and OR `TG.hookmask_th` with the global hook mask;
- hook count: keep the universe-global counter, but decrement it with an
  atomic C helper because the current ARM DynASM surface lacks a convenient
  exclusive/LSE RMW spelling.

Every path that establishes or reconstructs an assembler frame must establish
`x25` before dispatch or publication: resume, call/pcall, protected C call,
unwind-to-C, unwind-to-fast-function, coroutine return, callback entry/leave,
and C-function return.  Callee-save preservation restores the outer caller's
dispatch register across nested coroutine resumes.

JIT-only exit/redispatch and `jit_base` paths remain behind the enforced
`LUAJIT_DISABLE_JIT` boundary during this stage.

## Stage 2: stack and result publication

The conservative first ARM implementation should prefer reviewed rooted C
helpers and `lj_state_stack_pubtv(L, L, slot)` over speculative inline fast
paths.  A lightweight assembler-callable helper around
`lj_tg_stack_dirty_epoch_add_rlx()` is appropriate for range/frame operations
where every slot is already release-published.

Highest-priority correctness paths are:

- `vmeta_tgetv`/`vmeta_tsetv`: reload `BASE` after potentially relocating
  helpers and do not overwrite helper-published results;
- `cont_ra` and terminal `cont_cat`: store, dirty, and conditionally publish
  collectable roots;
- `TGETV`, `TGETS`, `TGETB`, `TGETR`, and `ITERN`: remove naked retiring table
  storage access in favor of rooted helpers and generation validation;
- call/return frame topology, `fff_res`, test-copy, MOV, CAT, constants,
  FNEW/TNEW/TDUP, ITERC, VARG, RET/RETM/RET0/RET1, and IFUNCV: add the x86
  dirty-epoch parity sites.

Owner-private stack loads do not all require `ldar`; state ownership/handoff
is their acquire boundary.  Shared closed-upvalue and helper-published cell
loads do require acquire semantics and are already handled for ARM UGET/CGET.

## Stage 3: safepoint behavior

ARM must acquire-load `poll` and `profile_request` as separate 32-bit words.
Do not copy the x86 combined qword test: the fields are published as distinct
32-bit atomics, and an overlapping 64-bit atomic access would create an
unnecessary mixed-width contract.

`vm_safepoint` saves `L->base` and `SAVE_PC`, calls
`lj_safepoint_ack_check(L)`, reloads `BASE`, and redispatches through the TG
table.  Required interpreter poll edges include:

- leave-to-C and unwind-to-C;
- ITERN/IITERL taken paths;
- ILOOP and JMP backedges;
- ordinary IFUNCF/IFUNCV entries, including interpreter execution inside a
  future JIT-capable binary;
- the numeric FORL/IFORL taken backedge, which x86 historically omitted but
  can otherwise run indefinitely without a C/native boundary.

JFOR/JITER/JLOOP, trace exit/rethrow, and XPOLL are deferred to the JIT phase.

## Validation gates

- Make `t-vm-safepoint.c` architecture-neutral and pass its loop, iterator,
  return, return-after-publication, and unwind/error cases on ARM.
- Add a pure numeric-for handshake case with no allocation, sleep, channel, or
  C call inside the loop.
- Strengthen the hook test with pure-Lua workers after the start gate.
- Add an observer fixture that acquire-samples TG `cur_L`/`vmstate` across
  coroutine handoff and Lua-to-C transitions.
- Add an ARM object contract requiring x25-based dynamic/static dispatch,
  separate `ldar` poll/profile reads, `stlr` TG state publication, and no
  active JIT-off `GG_G2DISP` access.
- Run native assert, ASan+UBSan, TSan, stock 387/387, coroutine/hook/threading,
  state-owner, and FFI callback suites.
- Cross-build x86-64 and verify `vm_x64.dasc` and the existing dispatch ABI are
  unchanged.

Implementation status: the interpreter portion of Stage 3 is implemented and
recorded in `aarch64-vm-safepoint-2026-08-26.md`. ARM64 signal-cache profiling
and every JIT/native-trace poll remain outside this checkpoint.

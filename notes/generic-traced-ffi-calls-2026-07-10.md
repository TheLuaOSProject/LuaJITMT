# Generic traced FFI calls on x86-64

Status: implementation design plus completed source generalization and
error-state precursors, 2026-07-10. The stock-derived generic scalar recorder
is now the only ordinary-C-call recording structure, but a deliberate hard
safety gate still prevents `IR_CALLXS` emission until the native publication
protocol is complete.

This is a deliberate divergence from the enumerated FFI-call-shape approach in
the plan. `plan/` is left unchanged. The replacement keeps the plan's safety,
locklessness, compatibility, and performance goals, but uses one ABI-driven
`IR_CALLXS` path instead of matching a C declaration against hundreds of C
wrappers.

The supported targets in this design are:

- Linux x86-64, System V ABI;
- macOS x86-64, System V ABI;
- Windows x64, tested under Wine as well as native-compatible builds.

`ffi.cdef()` concurrency remains out of scope, as requested. Recorder and
assembler code must nevertheless use CType snapshots because ordinary CType
growth can relocate the CType table.

## Decision and completed mechanical cleanup

The explicit bridge was not a viable long-term architecture. This checkpoint
removed, rather than merely bypassed:

- 137 `static` recorder declarations/definitions carrying 136 distinct
  `crec_call_jit_*` names (one count was a forward declaration), including the
  entire ordered shape-dispatch fan-out;
- 163 distinct `lj_ccall_jit_*` C wrappers;
- the matching 163 header declarations and 163 fixed IR-call table entries;
- all 45 `LJ_CCALL_JIT_*` signature constants.

`lj_crecord.c` fell from about 464 KiB to 96 KiB, `lj_ccall.c` from about
154 KiB to 46 KiB, `lj_ccall.h` from about 27 KiB to 6 KiB, and
`lj_ircall.h` from about 24 KiB to 14 KiB. The three independently audited
`lj_ffi_jit_{memcpy,memset,strlen}` fast helpers remain; they are not ordinary
C-call signature wrappers. The compact native save/enter/leave/checkstop
protocol also remains and is used by the interpreted call path.

The M7 source assertion requires zero explicit recorder, wrapper, or signature
names across the four production sources, exactly one `crec_call_args()`, and
exactly one generic scalar `IR_CALLXS` emission. The native-helper fixture now
tests `save/enter/leave/checkstop` directly instead of calling deleted wrappers.
The runtime trace-gate fixture still proves that a hot ordinary FFI call runs
correctly while producing no trace.

The replacement is:

1. one recorder that converts scalar, pointer, enum, and vararg arguments;
2. one direct `IR_CALLXS` using the existing x64 ABI lowering;
3. a compact per-TG native-call frame protocol around that direct call;
4. a snapshot-materialization marker before native publication;
5. a caller-state post-call exit guard;
6. a later ABI descriptor path for aggregate arguments and returns.

There must be no generic global SMR read section spanning the foreign call.
One sleeping FFI call would otherwise prevent all retired reclamation and
eventually exhaust the trace namespace. Later stages use an exact per-trace pin
instead.

## Non-negotiable invariants

The implementation is not complete until all of these hold.

1. A TG is published as remotely acknowledgeable native code only after every
   GC root needed by the suspended trace is in remotely readable storage.
2. Publication is ordered: stack stores, then release-publish the native frame
   as synchronized, then release-publish/increment `in_native`.
3. The synchronized frame remains published until `lj_native_leave()` has
   consumed any request and completed the final native-depth decrement.
4. A GC root handshake must not retire or unmap the machine-code return
   continuation.
5. A true trace flush must not retire the return continuation until it has an
   exact trace pin. Before that pin stage, flush is allowed to wait for the
   foreign call to return; mark-start is not.
6. A foreign call executes at most once. A post-call side exit must restore the
   caller after the call, never a snapshot which replays it.
7. `errno` and, on Windows, `GetLastError()` survive enter, leave, polling,
   blacklisting, trace exits, and immediate FFI result conversion.
8. Native/callback/STOPREQ state is a per-TG stack, not a process-global or
   shared `jit_State` scratch field.
9. CType pointers never survive an operation which can grow or replace the
   CType table. Copy `CType`, `CTInfo`, `CTSize`, and IDs as needed.
10. Public LuaJIT API and ABI remain unchanged. The removed `lj_ccall_jit_*`
    names are internal implementation symbols, not documented LuaJIT ABI.

## Stage 1: generic scalar `CALLXS`

### Recorder source

The snapshot-safe generic logic has been recovered and adapted rather than
rewriting its conversion rules from scratch. The authoritative last mixed
generic/early explicit baseline is the parent of `5053ce823`, i.e.
`5053ce823^` (`1c4853a167145865...`). The earlier `995c254f...` arena commit is
unrelated. The other useful stock-derived anchor is `304da39cc`:

- `crec_call_args()` around historical lines 2132-2258;
- `crec_snap_caller()` around historical line 2260;
- the generic part of `crec_call()` around historical lines 5159-5327.

`crec_call_args()` already supplies the important behavior:

- a maximum of `CCI_NARGS_MAX` (32) arguments;
- fixed-argument CType traversal through snapshot copies;
- vararg type inference through `lj_ccall_ctid_vararg()`;
- integer promotions and signed extension;
- scalar numeric, pointer, enum, float, and double conversion;
- a `CARG` tree in source order.

The compact portability branches were retained because they do not enumerate
signatures or burden the x86-64 hot path. Function type metadata is attached to
the function operand as the existing `CARG(func, typeid)` form for
varargs/calling-convention reconstruction. The unconditional `BLACKL` recorder
error is immediately before this single generic path, so no unsafe traced
foreign call can execute in this checkpoint.

The x64 backend already implements the scalar ABI details needed by this
stage:

- System V uses independent six-GPR and eight-XMM allocation and writes the
  used XMM count to `AL` for varargs;
- Win64 uses positional argument registers, reserves shadow/stack space, and
  duplicates FP varargs into the corresponding GPRs;
- macOS x86-64 follows the System V path;
- stack overflow beyond register arguments is already handled;
- `asm_callx_flags()` safely re-snapshots the function CType by ID.

The generic recorded sequence is conceptually:

```text
convert arguments and load function pointer
lj_snap_add(J)
IR_XSAVE
CALLS  lj_ccall_jit_enter(L, exact_trace, function)
CALLXS converted-arguments, function-plus-CType-metadata
CALLS  lj_ccall_jit_leave(L) -> postcall_exit
convert/box the foreign result
return from the FFI fast-function recorder
guard postcall_exit == 0 in caller state
```

`lj_ccall_jit_enter` may throw only before publishing native state (for example,
native-frame depth overflow). `lj_ccall_jit_leave` is a throwing IR call because
it performs the fresh-STOPREQ check after first restoring all native/callback
mirrors. Its normal return is zero or a bit saying that this trace must leave
compiled code.

The normal register allocator keeps the `CALLXS` result live across the leave
call. A first implementation may spill it or place it in a callee-save
register. The later inline fast path avoids this cost.

### Exact trace identity

Do not rediscover the trace from a reusable numeric slot once true flushes are
allowed during native execution. Reuse the existing trace-constant mechanism:

- allocate `lj_ir_ktrace(J)` once per trace and retain its ref in `J->ktrace`;
- pass that KGC trace constant to `lj_ccall_jit_enter`;
- the assembler already patches the placeholder to `J->curfinal`.

This gives the native frame the exact `GCtrace *` whose machine code contains
the call, including for side traces and after a public trace number is reused.
Before the pin stage the argument is useful for assertions and root marking;
after the pin stage it is authoritative.

### Result conversion

Keep stock generic conversions:

- `void` returns no result;
- signed/unsigned 8- and 16-bit results extend to Lua integer;
- unsigned 32-bit and float convert to Lua number where required by existing
  LuaJIT semantics;
- pointers, enums, and 64-bit integers are boxed with the exact return CType;
- double and ordinary signed 32-bit results remain native IR scalars.

C `_Bool` remains specialized to the observed path because LuaJIT IR has
guarded assertions, not a dynamic boolean value. Retain the small
`crec_snap_caller()` mechanism for that one purpose: its false-result caller
snapshot is exactly the non-replaying side exit for a boolean mismatch. This is
not declaration-shape matching and is only a few dozen lines.

### Post-call guard placement

A handshake/callback exit guard emitted inside `recff_cdata_call()` before
`lj_record_ret()` has the fast-function frame snapshot. Exiting through it can
re-enter/retry the FFI call or lose the already produced result. Do not do that.

Add `TRef postcall_exit` to `RecordFFData` and initialize it to zero in
`lj_ffrecord_func()`. The generic call recorder stores the normal return of
`lj_ccall_jit_leave()` there. Then:

1. call `lj_record_ret(J, 0, rd.nres)` as today;
2. add/merge a snapshot in the now-current caller state, even if
   `lj_record_ret()` has already selected a trace tail;
3. emit `EQ(postcall_exit, 0)` as a guard before that tail;
4. set `J->needsnap` as for other side-effecting fast functions.

The guard is unconditional in generated code. A selected tail is not
necessarily an interpreter exit: it can link to another trace. Omitting the
guard there would let a callback/handshake result link onward, leak the exact
trace pin, or make later cleanup rediscover the wrong body through a reused
trace number. `lj_snap_add()` can merge the just-created tail snapshot because
the guard is emitted immediately afterward. The leave helper is side-effecting
and cannot be dead-code eliminated. Boolean value specialization continues to
use its proven caller snapshot as described above.

Fresh STOPREQ is checked inside the leave helper before this guard. Thus a Lua
`pcall` catches the interruption with the established FFI-call semantics, all
native state has been popped, and the foreign side effect is not executed
twice. The helper must preserve the existing distinction between sticky
`TGF_STOPREQ` and one-shot `TGF_STOPREQ_FRESH` by using
`lj_safepoint_checkstop_fresh()` with the frame's entry snapshot.

### `IR_XSAVE`: publish roots without embedding a snapshot number

Add an operandless, side-effecting IR instruction named `IR_XSAVE` (the final
name is not ABI). The recorder must execute `lj_snap_add(J)` immediately before
raw-emitting it.

Do not encode `snapno` as an IR literal. Loop unrolling copies and reindexes
snapshots. A literal becomes stale. During backward assembly `as->snapno` is
already the correct snapshot for the current instruction, including the copied
loop body.

The x64 lowering is:

```c
assert(as->snapno < as->T->nsnap);
snap = &as->T->snap[as->snapno];
assert(snap->ref == as->curins);
asm_snap_prep(as);
asm_stack_restore(as, snap);
```

`asm_stack_restore()` alone is not a complete publication. The JIT's live
`RID_BASE`/`REF_BASE` can differ from the stale interpreter fields after
on-trace calls. At the same marker the lowering must derive the active frame
base with the snapshot frame links and publish both the stack-relative base and
top (or update `L->base`/`L->top` directly) before the native frame's release
publication. For a root-base pointer `rb`, the logical top is
`rb + snap->nslots - 1 - LJ_FR2`; the current frame base uses the same
`asm_baseslot()` rule as trace-tail restoration. Stack growth cannot occur
between these stores and native-frame publication.

The marker is also an allocation-sinking boundary. During `lj_opt_sink()`, any
snapshot whose `snap->ref` names `IR_XSAVE` must mark every referenced
allocation and dependent store non-sinkable. It is insufficient to rely on the
ordinary final/guard snapshot pass: loop snapshots are handled differently,
and `IR_XSAVE` has no operands through which the normal backward mark can reach
the owning allocation. Thus `asm_stack_restore()` never encounters a
`RID_SUNK` GC owner at a publication marker.

Add the normal fold identity for `XSAVE`. This matters because loop unrolling
re-emits side-effecting instructions through the fold pipeline. Snapshot refs
and the side-effect mode keep the marker ordered.

`asm_stack_restore()` already avoids rewriting readonly SLOADs whose correct
value remains in the Lua stack. The implementation must additionally verify
these root-publication cases before enabling the path:

- function cdata and all GC-owning argument slots are present in the XSAVE
  snapshot even if the caller will overwrite them after the call;
- a raw pointer derived from a string/cdata retains its owning GC object in a
  stack slot;
- a sunk allocation is either not materialized yet (and hence needs no GC
  root), or is forced fully unsunk, including its stores, before a pointer to it
  is published;
- local-cell `SNAP_NORESTORE` rules are not bypassed by blindly storing an
  internal cell pointer as a TValue.

For callback/debug-stack compatibility, the final implementation must expose
the complete logical outer frame, not only GC references. The simplest correct
first lowering is the full existing stack restore. A later optimization may
omit provably unchanged stores, but cannot make the first callback observe stale
locals.

The dormant x64 lowering and its assert fixture are now implemented. LOOP copy
substitution drains every snapshot due at an original IR reference and keeps an
XSAVE snapshot from being overwritten by the next unguarded snapshot. The
lowering stages the register which actually owns `REF_BASE`: backwards register
allocation deliberately ignores a restrictive allow set after a ref already
has a register, so pretending to force `RID_BASE` here (or rewriting `RETF` to
make that true) is invalid. The stack-restore emitter now accepts its base GPR:
ordinary trace tails pass `RID_BASE`, while XSAVE passes the exact SSA owner for
both materialization and TG staging. On non-dual-number builds, full
materialization can expose a
narrowed `IRT_INT` which ordinary exit-tail snapshots leave in the stack; XSAVE
widens it to a double before writing a Lua `TValue`.

`t-jit-xsave.c` now proves all of these relations from the finished trace body:
the pre-roll marker and LOOP-substituted marker each own a matching snapshot,
snapshot-owned allocations/stores are not sunk, an inlined Lua frame has a
nonzero base offset, and a separate down-recursive return trace has `IR_XSAVE`
strictly before a later `IR_RETF`. That last case is distinct from ordinary
inlining (an inlined return does not itself emit `RETF`). The unconditional
Lua-call and Lua-tailcall
recorder rejections inherited by this fork were removed; the normal M6 suite
also retains direct inlining and tailcall regressions independent of the XSAVE
test helper.

### Per-TG native-call stack

Separate C helper calls cannot keep `CCallNativeState` in a C automatic. Add a
fixed per-TG frame stack with the same maximum nesting as callbacks
(`CCALLBACK_MAX_NEST == LJ_MAX_XLEVEL`). A fixed array is allocation-free and
supports callback -> FFI -> callback recursion.

The first-stage logical frame contains at least:

```text
exact GCtrace pointer (initially diagnostic; later pinned)
function pointer
stack-relative saved jit base / synchronized snapshot identity
old ffi_call_func
old callback slot
old native_had_stopreq
had_stopreq on entry
entry forced-exit epoch
callback_seen / synchronized / active flags
transition sequence
```

Store stack positions as `savestack()` offsets if they can survive a callback;
a callback may grow and relocate the Lua stack.

Remote acknowledgement uses the transition sequence as a seqlock; the
sequence is not a mutex. Enter release-publishes an even stable sequence only
after `XSAVE` and the frame fields are complete. Callback entry and native
leave first publish an odd sequence before changing the Lua stack, mirrors, or
frame state, and publish the next even value after the transition. A remote
root scanner may acknowledge only when it reads the same even sequence before
and after acquire-loading the frame and stack. If it races a transition it
simply declines the remote acknowledgement: callback entry polls as an ordinary
Lua owner, and native leave reaches the generated post-call poll/guard. Neither
side waits for the other. This closes the otherwise possible race where a
callback starts mutating the just-materialized stack while a GC leader is
sampling it.

Enter ordering:

1. reserve/check the next owner-private frame;
2. copy old mirrors and fill the new frame;
3. set callback slot to `~0u`, publish `ffi_call_func`, and snapshot STOPREQ;
4. release-publish `synchronized = 1` and the frame depth;
5. call/release-publish `lj_native_enter(tg)` last.

Leave ordering:

1. call `lj_native_leave(L)` while the frame remains synchronized and visible;
2. if callback slot changed, blacklist the function and set `callback_seen`;
3. restore callback slot, `ffi_call_func`, and `native_had_stopreq` exactly;
4. compute forced-exit status from actions, callback state, and the per-TG exit
   epoch;
5. release-clear/pop the frame;
6. perform the fresh-STOPREQ check;
7. return the forced-exit status.

The existing automatic `CCallNativeState` API remains for interpreted calls and
can share internal push/pop primitives, but it must not be deleted.

## Stage 1b: non-retiring JIT-root handshake

GC mark-start currently uses `LJ_GC2_HS_EXIT_TRACES`. That action also leads to
trace retirement/quiescence and therefore cannot remotely acknowledge a traced
foreign call: the return PC is still in its mcode. Waiting for a sleeping C
function makes GC blocking.

Add a separate action, for example `LJ_GC2_HS_TRACE_ROOT_SYNC`, with these
semantics:

- enable barriers/black allocation and close trace redispatch as mark-start
  already does;
- make running traces leave at their next `XPOLL`/VM exit;
- release-store the handshake epoch to a per-TG `jit_force_exit_epoch`;
- remotely acknowledge a native TG with non-null `jit_base` only if its top
  generic native frame is synchronized;
- do not unlink a trace, reserve/release a trace slot, retire a body, or retire
  mcode;
- do not run trace-quiescence or consumed-poll holding intended for
  `EXIT_TRACES`/`FLUSHJ`.

Replace `EXIT_TRACES` with `TRACE_ROOT_SYNC` in GC mark-start. Keep true
`EXIT_TRACES` and `FLUSHJ` conservative until the trace-pin stage.

The frame captures `jit_force_exit_epoch` before native entry. Leave returns a
nonzero `postcall_exit` if the epoch changed, if it consumed any trace-exit
action, or if a callback occurred. This closes the race where the leader
remotely acknowledges the synchronized sleeper and completes mark-start before
the foreign call returns.

This split is the minimum safe way to make GC non-blocking without allowing a
leader to free the native return continuation.

## Stage 1c: conservative first-callback safety

A generic recorder will trace declarations which the shape bridge previously
left interpreted. Some of those functions may call an FFI callback for the
first time, before the blacklist has learned their address. Therefore the
generic path must not become the default, and the explicit bridge must not be
deleted, while `lj_ccallback_enter()` still treats synchronized non-null
`jit_base` as fatal.

The deletion gate needs a conservative callback suspension which does not yet
make true flush non-blocking:

1. On a same-TG callback, require a synchronized generic native frame; unknown
   non-generic `jit_base` cases remain an internal error.
2. Mark `callback_seen`, blacklist the foreign function, and release-publish an
   outer-suspended-trace record containing the exact trace constant and saved
   stack-relative JIT state.
3. Only after that publication, clear the outer `jit_base`, temporarily leave
   native depth, and poll before running callback Lua code.
4. Make trace quiescence and true `EXIT_TRACES`/`FLUSHJ` treat the suspended
   record exactly like non-null `jit_base`. Thus a flush still waits; it cannot
   retire the unpinned continuation.
5. Run callback Lua/JIT normally. Callback leave restores outer JIT/native
   state, and the outer post-call guard exits because `callback_seen` is set.
6. Callback unwind clears/restores the same state on every error path.

This makes the first callback natural and safe using the already synchronized
stack, while keeping retirement conservative. Stage 3 replaces the
flush-blocking suspended marker with an exact body pin and then permits remote
flush acknowledgement.

## Error-state preservation

The completed precursor preserves both C `errno` and Win32 `GetLastError()` at
the outermost boundary of:

- `lj_ccall_native_enter()` and `lj_ccall_native_leave()` (fixing interpreted
  calls and all existing wrappers as an independent first patch);
- generic JIT enter and leave helpers;
- any callback suspend/resume slow path;
- any post-call slow path added by the inline lowering.

`lj_ccall_native_save()` captures before interpreted argument conversion and
`lj_ccall_native_enter()` restores that exact pair after publishing native
state. Thus allocation while preparing arguments cannot silently change the
error state observed by the foreign function. `lj_ccall_native_leave()` saves
and publishes the foreign pair as its first operation, leaves native state,
restores all surrounding callback/function mirrors, and only then runs the
potentially yielding callback-blacklist path bracketed by restores. The
fresh-STOPREQ check likewise starts from the foreign pair. The snapshot lives
in the stack-local `CCallNativeState`; nested calls have independent frames.
The frame stores the owning TG rather than a redundant callback-runtime pointer;
enter/leave validate that TG against `L` and derive `tg->cb`, tightening the
ownership invariant without an optimizer-dependent null-object store.

The nonlocal STOPREQ throw is covered too. Error message construction, error
handler setup, trace abort, Lua-stack unwinding, and callback-frame cleanup
carry the current pair in `ErrOSState` C automatics and restore it at each
unwind boundary. A callback unwind may deliberately replace that automatic
with the pair selected by its callback frame. There is no TG/global pending
slot. An `xpcall` handler observes the pair at the error edge; if the handler
explicitly changes it, its outgoing pair is retained.

Post-call boxing does not use a persistent per-TG pending slot. Instead,
`lj_cdata_new_forjit()` preserves the current pair around every materialized
`CNEW`/`CNEWI`, and `lj_cdata_newv()` does the same for VLA/VLS/over-aligned
materialization. Both use the non-throwing allocator primitive, restore the
pair before an OOM edge, and only then enter `lj_err_mem()`; the latter carries
that pair across error-string/stack repair to the actual nonlocal throw. The
interpreted path restores its stack-local call snapshot after result conversion
and requested GC steps. This is both more general and safer than a single
pending slot: arbitrary nesting cannot overwrite it, a side exit cannot leave
it armed, and there is no state for the next FFI call on the TG to inherit.

The external unwinder now carries the pair through its own transport too.
POSIX `LJErrUEx` keeps `global_State *g` at the historical `(uex + 1)` offset
and appends the pair plus an x64 landing carrier; compile-time layout checks
lock those offsets. A cleanup-function marker distinguishes this extended
object from an older LuaJIT DSO using the same exception class, avoiding an
out-of-bounds tail read while retaining the old `g` ABI. Win64 SEH stores the
pair in `EXCEPTION_RECORD::ExceptionInformation` and carries it to dedicated
C/FF landing labels in `R10`.

Restoring inside a personality is insufficient by itself because neither
libgcc/libunwind nor `RtlUnwindEx` promises to preserve platform TLS between
personality return and context installation. On x64, the personality replaces
callee-saved `R12` with a pointer to an exception-owned carrier after saving its
trace value. `vm_unwind_os_eh` pushes the real target, preserves RFLAGS, all 15
non-RSP GPR values and XMM0-XMM15, restores errno/LastError, restores every
register including the original `R12`, and returns with the exact original
target RSP. Win64 direct JIT unwind uses a TLS carrier with no armed/pending
state; normal exits pay no cost. The JIT remains SSE-only, so AVX upper halves
are intentionally outside this context contract. `lj_trace_exit()` already
brackets exit reconstruction with `ERRNO_SAVE`/`ERRNO_RESTORE`, and the C
landing's possible `lj_safepoint_ack_check()` is now error-transparent through
its STOPREQ throw boundary.

Callbacks use the same ownership rule. Prepare/enter preserve the incoming
native pair, and the callback frame stores it as soon as the FFI continuation
exists. Normal leave replaces the frame copy with the Lua callback's outgoing
pair before result conversion. A body error therefore unwinds with the entry
pair, while a result-conversion error unwinds with the outgoing pair. Popping
the frame clears both fields, so nested callbacks and errors leave neither an
error-state slot nor callback/native mirror residue.
The x64 trampoline's intervening `lj_ctype_ctsG_acq()` call is explicitly
error-transparent too; correctness does not rely on that accessor remaining a
single compiler load or on instrumentation leaving platform TLS untouched.

The existing declaration-shape wrappers currently evaluate `ctype_cts(L)`
before their stack-local save. That accessor is an inline acquire-load with no
wait, allocation, or external call. The interpreted `lj_ccall_func()` path is
different: CType snapshot retries can yield, so it snapshots the error pair at
function entry before even loading/waiting for the CTState entry.

`t-ffi-ccall-error-state.c` installs a custom allocator which deliberately
replaces both values. Its default path proves normal interpreted pointer
conversion and a forced post-return fixed-cdata OOM both carry the foreign pair
through the allocation edge and unwind. The longer boxed `CNEWI`, over-aligned
`CNEW`, and VLA matrix remains behind `LJ_FFI_ERRSTATE_ALLOC_STRESS`: ordinary
FFI calls are recorder-gated in this checkpoint, and GC2 does not yet have the
custom-allocation registry needed to keep a `lua_setallocf()` wrapper installed
across new collection cycles. That allocator ABI blocker is documented in
`gc2-only-runtime-migration-2026-07-10.md`; the stress must become a default gate
when it is fixed.
`t-ffi-ccall-native-helpers.c` publishes a concurrent REDISPATCH while the
callee is native and verifies the immediately captured `EDOM` survives leave.
Its STOPREQ case also installs a deliberately clobbering allocator while the
error is formatted and checks both values immediately after `pcall`, proving
that an unreachable post-check restore is not being mistaken for coverage.
It also forces the first post-return fixed-cdata allocation to fail and verifies
that `pcall` observes the foreign pair after the OOM unwind. A test-only
`LJ_OSERR_TEST_UNWIND_CLOBBER` build deliberately replaces the pair after every
personality return; the OOM and callback fixtures still pass, directly proving
the final landing restore rather than relying on current libgcc behavior.
`t-ffi-callback-nested-native.c` covers callback-to-FFI-to-callback propagation,
body-error unwind, result-conversion-error unwind, and empty callback-frame
state afterward. Windows builds run the same allocator test against
`GetLastError()`.

The Win64 R10 target-context path and direct-JIT TLS carrier compile in the
cross build but still require Wine execution in the integration matrix. Foreign
MSVC/C++ exceptions do not carry this fork's Lua exception payload; their own
destructors may choose new errno/LastError values, so only Lua-originated error
transport has the exact throw-edge guarantee described above.

## Stage 2: enumerated bridge removed behind the safety gate

This mechanical stage was completed before enabling generic calls. That
ordering is safe because the hard recorder gate still routes every ordinary C
call through the interpreted native-state path: neither the deleted wrappers
nor the dormant generic `IR_CALLXS` can execute from a trace. Removing the
generated surface now prevents new shape-specific dependencies from growing
while the native-frame protocol is implemented.

The deletion was performed by symbol boundary, not brittle line numbers:

- remove every `crec_call_jit_*` signature matcher and the large sequential
  matcher dispatch from `src/lj_crecord.c`;
- remove every `lj_ccall_jit_*` declaration-shape wrapper from
  `src/lj_ccall.c`;
- remove the `LJ_CCALL_JIT_*` signature enums/macros and wrapper declarations
  from `src/lj_ccall.h`;
- remove all 163 corresponding entries from `src/lj_ircall.h`;
- retain `lj_ccall_func()`, `CCallState`, the interpreter ABI classifiers,
  callbacks, and the compact native enter/leave primitives;
- retain only compact, declaration-independent helpers.

Lifting the hard gate remains conditional on the generic scalar matrix, the
new root/STOPREQ/error tests, and conservative first-callback suspension
passing on Linux release and assert builds. Arbitrary generic calls must not be
exposed while first callback entry can terminate the process.

Keep `tests/t-ffi-ccall-native.lua` and
`tests/t-ffi-ccall-jit-lib.c` initially. Although large, they become valuable
proof that arbitrary declarations use one generic path. Once the replacement
has soaked, convert the repetitive cases to a generated/table-driven boundary
matrix; do not lose ABI register-boundary coverage merely to reduce test source.

## Stage 3: callback-safe suspended traces and non-blocking flush

The present `lj_ccallback_enter()` aborts/panics when `jit_base` is non-null.
Removing that check without preserving the outer trace is use-after-free. The
safe final protocol is:

1. Generic enter atomically pins its exact `GCtrace` while `jit_base` still
   prevents concurrent retirement.
2. The synchronized native frame publishes the pinned body, trace number,
   stack-base offset, and callback state.
3. A same-TG callback marks `callback_seen`, blacklists the current foreign
   function, saves outer `jit_base`/vmstate as stack-relative state, clears the
   active outer-trace publication, sets native depth to zero, and polls.
4. Callback Lua code then runs normally, including nested JIT and nested FFI.
5. Callback leave restores the suspended outer native continuation and native
   depth. The outer foreign function returns to mcode, and the post-call guard
   immediately exits that trace.
6. Callback error/unwind performs the same restoration or releases the pin if
   the outer continuation will never resume.

Callbacks on another OS thread have no outer trace frame on that TG; they use
the existing attach/carrier path. Same-TG blacklisting is an optimization, not
the safety mechanism.

Add an atomic `native_pins` count to `GCtrace`. A retired body with a nonzero pin
stays on the preserved retired-body list, which also makes
`lj_trace_retired_mcode_refs()` retain its mcode area. Do not keep a global SMR
reader open.

The first pin implementation may keep the retired trace slot reserved until
the pin reaches zero. The complete implementation should release the public
slot after grace even while the body remains pinned, so long sleepers do not
exhaust `maxtrace`. At that point:

- exit restoration must use the TG's exact pinned `GCtrace *`, not a possibly
  reused trace slot;
- the pin is retained through snapshot restore and released only after the
  outer guard/tail has reached interpreter-safe state;
- `gc2_scan_jit_roots()` marks pinned/suspended trace bodies, their KGC/proto
  graph, and all suspended outer frame roots;
- VM interpreter-exit cleanup releases a pending pin for traces which ended at
  a natural tail rather than a guard;
- reclaim requeues a grace-ready body while `native_pins != 0`, but may free its
  old slot; mcode reclaim continues to see the requeued body.

True `EXIT_TRACES`/`FLUSHJ` may remotely acknowledge a traced native TG only
when the top frame is synchronized and holds this exact pin. This makes
`jit.flush()` return while the foreign call remains blocked without freeing its
return continuation.

### Win64 trace-exit prerequisite

Completed. System V passes `L`, parent trace number, and exit number as separate
arguments. Win64 keeps its four-register ABI by packing the two public 16-bit
IDs into one argument. Neither path writes `J->L`, `J->parent`, or `J->exitno`,
and both use the currently executing TG's `jit_exitcode`.

The protected restore path calls `lj_snap_restore_exit()` with that explicit
descriptor on every x64 ABI. This last step is essential: leaving Win64 on the
legacy `lj_snap_restore()` entry would still re-read the shared recorder fields
after the exit stub stopped publishing them. Repeated guarded-side-exit coverage
is shared by Linux, Wine Win64, and Darling macOS suite runs. The pinned-body
override can therefore use the same explicit exit descriptor on all three
targets.

## Stage 4: aggregates, complex values, and vectors

Aggregate calls may initially remain interpreted. The interpreter already
enters a native region, is thread-safe under the same native protocol, and is
non-blocking; this is the stock recorder fallback and does not require keeping
the enumerated scalar bridge.

There are two useful aggregate milestones.

### 4a. Generic helper-backed aggregate call

Materialize the FFI call frame and invoke one generic helper which reuses
`ccall_set_args()`, `lj_vm_ffi_call()`, and `ccall_get_results()`. This removes
all declaration matching and allows aggregate-heavy traces to cross a single
generic side-effecting operation, though it is not yet a direct ABI call.
All CType access remains snapshot/ID based.

### 4b. Direct ABI descriptor lowering

Extract/share the x64 ABI classifiers in `lj_ccall.c` into a pure descriptor
builder. The recorder produces an immutable `ABICallDesc` containing copied
facts only--never movable `CType *` pointers. The descriptor belongs to the
trace body and therefore follows trace retirement/pinning.

System V descriptors must represent:

- one/two eightbyte INTEGER and SSE classes (and SSEUP when vector support is
  added);
- MEMORY for size, packing, or unaligned members;
- atomic register-allocation rollback: if every class of one aggregate does not
  fit, the entire aggregate goes to the stack;
- stack size/alignment and hidden sret argument;
- complex and vector cases supported by the interpreter ABI.

Win64 descriptors must represent:

- 1/2/4/8-byte aggregates passed/returned as integer values;
- larger aggregates copied to stable temporary storage and passed by reference;
- strictly positional GPR/XMM allocation, shadow space, and stack arguments;
- hidden sret shifting of user argument positions;
- FP vararg duplication into the matching GPR.

Preallocate aggregate-result cdata before `XSAVE`. Its payload pointer is the
hidden sret argument when needed, and the cdata must be rooted in the published
stack. For register aggregate returns, have the backend copy
`RAX/RDX/XMM0/XMM1` directly into that preallocated payload. Trying to encode a
multi-register aggregate as unrelated scalar `CALLXS` results loses grouping
and rollback rules; use a descriptor-aware call IR/lowering instead.

Aggregate arguments likewise need descriptor-directed outgoing stack copies.
Do not model each eightbyte as an independent C argument: that breaks System V
atomic rollback and Win64 positional semantics.

## Performance path

The correctness implementation uses two fixed helper calls plus a direct
foreign call. It should already remove a large wrapper call graph and major
I-cache cost, but a no-op C function can expose result spilling across leave.

After correctness gates, teach the x64 backend to inline the owner-only fast
enter/leave sequence:

- inline fixed-frame push/mirror stores and synchronized/native publication;
- after the foreign call, test callback marker and `poll`;
- when both are clear, decrement/restore/pop inline without clobbering
  `RAX/RDX/XMM0/XMM1` or errno/Win32 last error;
- call the slow helper only for a poll, callback, STOPREQ, pin transition, or
  exceptional nesting case.

Because TG state has one owner, remote threads only acquire-load it; the fast
path need not introduce a process-wide lock. Preserve the release/acquire
publication contract.

Performance gates:

- correctness stage: no more than 15% regression against the current explicit
  bridge for a no-op leaf loop, and no material regression for libc-sized work;
- inline stage: within 3% of stock/current no-op throughput, with parity or an
  improvement for mixed signatures due to smaller text/I-cache footprint;
- measure Linux System V, Wine Win64, and Darling macOS binaries with JIT on/off,
  not wall-clock test-suite noise alone.

## Test gates

Do not delete the explicit bridge until gates 1-8 pass. Do not enable remote
flush acknowledgement until gates 7-10 pass.

1. Existing scalar matrix: all current fixed signatures trace through the same
   generic recorder, including signed/unsigned narrow, i64/u64, pointers,
   float/double, void, ignored results, and 0-32 arguments.
2. ABI boundaries: System V six GPR/eight XMM plus stack and mixed reordering;
   Win64 four positional registers, shadow space, stack spill, and mixed types.
3. Varargs: custom C varargs with integer promotions, doubles, high-bit unsigned
   values, System V `AL`, and Win64 FP duplication.
4. Root publication: the only references to a table, string, closure, cdata,
   weak key, and weak value are on-trace locals while C blocks; a peer completes
   full GC; all values and frame functions survive.
5. Non-blocking mark-start: a traced C sleep/block remains in native code while
   another TG completes mark-start/full collection within a bounded deadline.
6. STOPREQ/error: `pcall` catches fresh STOPREQ, the C side-effect counter is
   exactly one, native depth/mirrors/frame depth are restored, and errno plus
   Win32 last error survive a concurrent handshake.
7. First same-TG callback on a trace works naturally, marks the function for
   future interpreted calls, and forces a post-return exit; include nested
   callback -> FFI -> callback.
8. Callback error, STOPREQ, stack growth, autoattach, and unwind leave no frame,
   pin, `jit_base`, callback slot, or `ffi_call_func` residue.
9. `jit.flush()` completes while a traced C sleep is still blocked; return exits
   safely after the sleeper is released.
10. Small `maxtrace` churn repeatedly retires/reuses slots and mcode while one
    old trace is pinned; run assert, ASAN, and UBSAN builds.
11. Aggregates: sizes 1/2/4/8/9/16/>16, alignments, packed/unaligned fields,
    mixed INTEGER/SSE, register exhaustion rollback, sret, complex, vectors,
    and Win64 by-reference copies.
12. Compatibility: upstream LuaJIT API/ABI suites, full repository tests, debug
    API inspection from a callback, Linux release/assert, Wine, and Darling.

Useful failure assertions include: XSAVE snapshot ref equals current IR,
published native implies synchronized frame, synchronized frame has a live Lua
stack, pinned trace contains the return PC, pin count never underflows, and no
retired body is freed while pinned.

## Smallest implementation slice

The first source patch series should be limited to the following, in order:

1. Preserve `errno`/Win32 last error around existing interpreted native enter
   and leave; add focused tests.
2. Add the fixed per-TG generic native frame stack and two generic helpers, but
   do not remove any old wrappers.
3. Add `IR_XSAVE` with snapshot-relative x64 lowering and root-publication
   assertions/tests.
4. Restore scalar/pointer/enum/vararg `crec_call_args()` and emit generic
   `CALLXS`; add caller-state `RecordFFData.postcall_exit` handling.
5. Add `TRACE_ROOT_SYNC`, switch mark-start from retiring `EXIT_TRACES`, and
   prove a traced blocking call no longer stalls GC.
6. Add conservative synchronized callback suspension, with true flush still
   treating the suspended outer trace as active.
7. Run the full existing scalar/callback matrix on Linux; only then delete the
   explicit recorder/wrapper/IR-call blocks in a separate, reviewable commit.

This slice intentionally leaves aggregate calls interpreted, true `jit.flush()`
conservative, and trace-slot reuse across a sleeping call disabled until the
exact trace-pin stage lands. It still replaces declaration-shape matching,
makes traced scalar FFI and first callbacks safe, and removes GC's dependency
on a sleeping traced foreign call without risking return-mcode reclamation.

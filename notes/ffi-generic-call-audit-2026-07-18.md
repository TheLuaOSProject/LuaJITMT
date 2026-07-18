# Generic traced FFI call audit (2026-07-18)

Audit point: `6e339863` on x86-64. This note records the current source state and
the intended route forward. It does not change the immutable plan.

## Result

The production "match this C declaration against an explicit supported shape"
architecture is already gone. Commit `830297de` deleted the generated
`crec_call_jit_*` recorders, `lj_ccall_jit_*` native wrappers, and
`LJ_CCALL_JIT_*` IR-call catalogue. Current production source has zero instances
of those three families.

Do not recreate that architecture and do not add another declaration/signature
allowlist. Ordinary traced C calls must use generic ABI classification and one
generic native-transition protocol.

The many `lj_m7_ccall_jit_*` names that remain are test-library exports, not
runtime dispatch entries. The old per-shape notes are historical records; the
archive banners in `ffi-callxs-record-gate.md` and
`ffi-ccall-native-helpers.md` already identify them as superseded.

## Generic source seam and current gate

There is one generic scalar recorder path:

- `src/lj_crecord.c`: `crec_call_args()` records numeric, enum, and pointer
  arguments, including vararg promotions, as a generic `CARG` tree.
- `src/lj_crecord.c`: `crec_call()` contains the sole `IR_CALLXS` emission.
- `src/lj_asm.c` reconstructs the argument list and CType metadata.
- `src/lj_asm_x86.h` performs generic x64 ABI lowering. SysV has independent
  GPR/XMM allocation and vararg `%al`; Win64 has positional register slots,
  stack/shadow-space handling, and FP-vararg GPR duplication.

This seam is deliberately unreachable today: `crec_call()` raises
`LJ_TRERR_BLACKL` unconditionally before loading the function, classifying the
result, or emitting `IR_CALLXS`. Consequently ordinary C calls run through the
interpreter even though generic scalar lowering exists.

Do not merely remove this gate. Scalar lowering is not yet surrounded by the
GC/JIT/native-transition protocol required for a blocked foreign call.

## Useful prerequisites already present

- `IR_XSAVE` records and lowers a snapshot-relative full Lua stack restore.
  Its `ffi_xsave_root`, base-slot, and slot-count fields are owner-private
  staging for a future generic native-enter operation.
- `lj_ir_ktrace()` plus `J->ktrace` provides an exact trace-body constant that
  the assembler patches to the finalized trace.
- `RecordFFData.postcall_exit` and its caller-state guard exist, but no current
  recorder sets the field.
- The interpreted `CCallNativeState` enter/leave/check-stop protocol preserves
  errno/GetLastError, STOPREQ, callback blacklist state, and unwind state.
- Retired trace-body and mcode-area lists already support delayed reclamation;
  an exact pin can be added to those readiness predicates.

## Blocking correctness work

The gate cannot open until all of these are true:

1. A per-thread-group, sequenced, preallocated native-call frame publishes the
   exact trace, restored Lua root interval, function, nesting state, saved VM/JIT
   mirrors, and entry epochs before incrementing `in_native`. Publication and
   removal must allocate nothing and wait for no peer.
2. Remote GC root scanning can double-read an even sequence, scan that stable
   frame, and validate the sequence again. A traced foreign-call sleeper must be
   accepted as quiescent-for-mark; the current `lj_tg_any_jit_active()` gate
   instead prevents a GC cycle from starting or closing while it sleeps.
3. `GCtrace` has an exact atomic native pin. Retirement and mcode reclamation
   must retain pinned bodies. Trace-number reuse must never redirect a return to
   a different body; the first implementation may keep the public slot reserved
   until the pin reaches zero.
4. Native leave preserves the ABI result and OS error state, reconciles GC/flush
   epochs and STOPREQ, and either releases the pin normally or leaves an exact
   pending-exit cleanup record. Side exit or unwind must release every frame and
   pin exactly once. The post-call recorder guard must force that exit without
   replaying the C call.
5. Callbacks suspend the outer native/JIT frame through the same sequence
   protocol, run Lua, restore it, mark `callback_seen`, and force a post-call
   side exit. The current callback path fatally rejects active JIT state.
6. TLS-less and cross-universe callbacks need independently leasable rooted
   carriers and reversible TLS binding. The present single hidden carrier plus
   `lj_threading_attach_wait()` serializes concurrent callback entrants and is
   not a lockless solution.
7. Aggregates need a trace-owned immutable ABI descriptor, not shape matching:
   SysV eightbyte INTEGER/SSE/SSEUP/MEMORY classification and rollback, and
   Win64 size/by-reference/sret/positional rules. Multi-register results need a
   descriptor-aware call operation; scalar `CALLXS` alone is insufficient.

There is also pre-existing Win64 callback-unwind debt independent of generic
tracing. A production-object UCRT/Wine run of
`tests/t-ffi-callback-nested-native.c` completes the normal nested callback and
errno stage, but its first callback-body error inside `pcall` returns
`LUA_ERRERR` (`error in error handling`). Clean b1.2.0 and the current
LastError-transparent GC2-cell build fail at the same stage, including a
DLL-linked diagnostic, so this is neither a static-link artifact nor a
regression from the GC2 migration. Make that fixture a native/Wine FFI gate
when the callback error-unwind path is repaired; it cannot yet be an
expected-green GC2 storage gate.

The native frame should use an odd/even publication sequence. Enter consumes
the owner-private XSAVE staging, pins the exact trace while the current
`jit_base` still protects it, release-publishes an even frame, and only then
enters native state. Leave first captures errno/GetLastError and closes native
state while the frame is still stable, then decides normal return versus forced
exit. No long-lived global SMR reader should span the foreign call.

GC mark admission must distinguish an acknowledgeable, stable pinned native
frame from an unpublishable active trace. Trace retirement/flush may acknowledge
the sleeper only after the exact body is pinned and must arrange an exact
post-return exit.

## Test fixture policy

`tests/t-ffi-ccall-native.lua` and `tests/t-ffi-ccall-jit-lib.c` are large
because they preserve the old signature catalogue as an ABI behavior oracle.
Under `LJ_M7_FFI_CCALL_GATE=1`, `expected_trace_count()` currently asserts that
each ordinary C call produced zero traces; the result and side-effect checks
still exercise the interpreted ABI path.

Do not delete this fixture before the generic path has parity. As generic traced
calls land, change the trace assertions from the inverted gate expectation to
real trace coverage. Then replace the hand-enumerated symbols in a separate
change with generated/table-driven ABI-class boundary cases:

- SysV GPR 5/6/7 and XMM 7/8/9 boundaries, independent mixed exhaustion,
  overflow stack alignment, vararg promotions, and `%al` counts;
- Win64 positional 4-register boundary, shadow/stack layout, FP-vararg
  duplication, sret shifts, and aggregate-by-reference cases;
- 0 through 32 scalar arguments, pointer/integer widths, and return classes;
- aggregate sizes/classes, packing/alignment, register rollback, complex/vector
  cases, and hidden returns;
- a blocked traced call during full GC, STOPREQ, callback, error, flush,
  maxtrace/slot-reuse, retirement, and mcode-reclamation stress.

Keep the `tests/suites/m7_ffi.lua` source assertions that explicit production
matcher/wrapper/catalogue families remain absent.

## Safe activation order

1. Native frame publisher/scanner, exact trace pins, retirement integration,
   and unit/assert fixtures while the recorder gate remains closed.
2. Opt-in Linux SysV scalar tracing with generated ABI boundary coverage.
3. Callback suspension, nested/cross-universe/concurrent TLS-less callback
   coverage, and exactly-once unwind cleanup.
4. GC/flush/slot-reuse/mcode-retirement stress with sleepers.
5. Win64 scalar parity under Wine, including LastError and varargs.
6. Descriptor-driven aggregate argument and result support.
7. Remove the unconditional blacklist and make generic tracing the default only
   after the above correctness barriers pass. Refactor the legacy fixture after
   parity, never as a prerequisite for implementation.

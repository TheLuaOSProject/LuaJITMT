# Temporary explicit FFI C-call safety gate

Ordinary FFI C calls are temporarily refused by the recorder. This is a safety
and performance divergence from the enumerated `lj_ccall_jit_*` bridge; the
plan files are unchanged.

The explicit wrappers publish the executing TG as `in_native` without first
materializing the trace snapshot. A GC handshake is then allowed to treat the
TG as remotely acknowledgeable even though `L->base`/`L->top` can still be
stale and GC-owning values can exist only in registers or as sunk allocations.
This is not limited to two simultaneously executing Lua TGs: a GC worker can be
enabled after a trace was created, and trace retirement also needs an exact
continuation pin rather than an activation-flush assumption.

`crec_call()` therefore raises the normal recorder blacklist error before the
single stock-derived generic scalar recorder. Interpreted FFI calls retain their native-state,
callback, STOPREQ, and errno/LastError handling. `ffi.copy`, `ffi.fill`, and
other separately audited FFI fast functions are unaffected. The large matcher
and wrapper blocks have been deleted, so this checkpoint is safe but
intentionally gives up traced ordinary-C-call performance.

## Removed matcher architecture

The former tree had 137 recorder declarations/definitions carrying 136 unique
`crec_call_jit_*` names, 163 `lj_ccall_jit_*` wrappers, 163 declarations, 163
IR-call entries, and 45 signature constants. All are absent from the four
production sources. Focused native-state tests now call only the compact
protocol directly; no internal wrapper entry point remains in the runtime.

Generalizing the matchers one signature family at a time would retain the wrong
boundary: C still cannot invoke an arbitrary mixed SysV/Win64 signature through
one ordinary C prototype, and a descriptor-marshalling trampoline would add
memory traffic while duplicating the ABI logic already present in the x64
`IR_CALLXS` backend. The safe seam is therefore one generic recorder plus the
existing backend, followed by mechanical deletion of the whole dormant fan-out.

The replacement seam is deliberately concentrated at `crec_call()`:

1. restore the snapshot-safe generic `crec_call_args()` and scalar result
   conversion from `5053ce823^`;
2. consume `IR_XSAVE` through a bounded per-TG native-frame stack, publishing
   the exact base/top/root range before `in_native`;
3. emit one ABI-driven `IR_CALLXS`, using the existing SysV and Win64 x64
   lowering, with an exact trace/mcode pin;
4. capture errno/LastError immediately in the leave helper and emit the
   post-call exit guard in caller state so the foreign call cannot replay;
5. exercise scalar, enum, vararg, and register-overflow signatures through
   that single path. The dormant production matchers, wrappers, declarations,
   signature constants, and IR-call entries are already deleted.

The focused `t-ffi-ccall-trace-gate.lua` regression proves that a hot ordinary
FFI C-call loop stays interpreted while this boundary is in force. The full
legacy shape matrix still runs every result/side-effect assertion under the
narrow `LJ_M7_FFI_CCALL_GATE=1` test mode. In that mode only its 320 historical
positive trace-count checks are inverted into no-trace safety assertions;
unmatched-call and separately audited fast-function expectations keep using the
real trace count. This retains the matrix as an ABI oracle for the generic
bridge without pretending the dormant wrappers execute in this checkpoint.

## Error-state audit follow-up

Callback frames now distinguish setup, Lua-body, and result-conversion phases.
Setup failure restores the incoming native pair; result conversion restores the
pair captured immediately after the Lua body; a body error retains its actual
throw-edge pair, including explicit `ffi.errno()` or Win32 `SetLastError()`
writes. `t-ffi-callback-nested-native.c` covers all three ownership rules and
empty-frame cleanup.

Both exceptional-path guarantees are now implemented as generic error-runtime
machinery, independently of the still-gated traced-call bridge:

- POSIX `LJErrUEx` carries the errno/LastError pair without changing the
  historical `global_State *` offset. The x64 install-context path transfers it
  through an exception-owned carrier to a register-preserving landing shim;
  Win64 SEH uses `ExceptionInformation` and an R10 landing carrier. Both paths
  restore after the system unwinder has installed the final context, rather
  than assuming platform TLS survives personality return.
- fixed/VLA cdata allocation saves the foreign pair before allocation and
  restores it before either normal conversion or `lj_err_mem()`. Consequently
  a failing post-call boxing allocation throws with the foreign pair, and the
  unwind carrier preserves it through `pcall`.

`t-ffi-ccall-error-state.c` covers allocation failure, STOPREQ, callback
nesting, and a fault-injection build which deliberately clobbers error state
between personality return and landing. Linux execution and the Win64 cross
build pass; the Win64 landing path still needs Wine execution in the integration
matrix. None of this weakens the recorder gate: native-root publication, exact
continuation pins, callback suspension, and aggregate ABI support remain the
prerequisites for enabling the single generic `IR_CALLXS` path.

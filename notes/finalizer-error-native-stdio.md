# Finalizer error native stdio

Date: 2026-06-20

## Problem

The default `ERRFIN` VM-event handler in `lib_aux.c` reports finalizer callback
errors to `stderr`. Unlike panic output, this path runs during normal GC
finalizer dispatch with a live `lua_State *`, but it still wrote and flushed
stdio directly.

That could leave a mutator blocked in diagnostic I/O without marking its TG as
native, delaying soft handshakes and STOPREQ acknowledgement.

## Fix

The finalizer-error reporter now enters native state around the stderr writes
and flush, then leaves native state. The leave action is intentionally not
thrown from inside the `ERRFIN` handler: the handler is itself a VM-event
callback, and converting STOPREQ into an error there would make
`lj_vmevent_call()` treat shutdown as a failing event handler before restoring
event-call state. The native leave still acknowledges the handshake and records
the sticky STOPREQ flag on the TG.

## Guard

`m8_finalizer_error_native_stdio` invariant: raw finalizer-error stdio
outside `aux_finalizer_error_report()`, verifies that the helper enters/leaves
native state, and runs `m8_finalizer_error_native_stdio`, which triggers an
erroring FFI finalizer and checks the default reporter output.

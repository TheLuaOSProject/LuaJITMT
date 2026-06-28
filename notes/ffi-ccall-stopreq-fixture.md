# FFI C-Call STOPREQ Fixture

Added `tests/t-ffi-ccall-stopreq.c` to pin interpreted FFI C-call native-state
freshness behavior.

The fixture covers two cases:

- a pre-existing sticky `TGF_STOPREQ` does not interrupt a successful
  `ffi.C.getpid()` call;
- a helper pthread waits until `ffi.C.poll(nil, 0, timeout)` has entered native
  state, publishes a real `LJ_GC2_HS_STOPREQ` handshake, and verifies the call
  leaves native state, restores `TGState.ffi_call_func`, and reports the
  shutdown interruption through Lua `pcall()`.

This strengthens the default safety bridge while `LJ_FFI_RECORD_CALLS` remains
off and `IR_CALLXS` awaits an explicit native-state protocol.

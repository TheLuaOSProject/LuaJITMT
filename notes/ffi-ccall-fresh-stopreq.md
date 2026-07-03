2026-06-27

- Routed interpreted FFI C-call native exits through a fresh STOPREQ helper in
  `lj_ccall_func()`.
- The check still runs after callback blacklist cleanup, TG `ffi_call_func`
  restoration, and result conversion, preserving the existing cleanup ordering.
- A pre-existing sticky shutdown flag no longer interrupts an otherwise
  successful `ffi.C.*` call when no new STOPREQ action was acknowledged by the
  native call.
- Added M3 handshake coverage for sticky `ffi.C.getpid()`. The old source
  notes document why we avoiding raw `lj_safepoint_checkstop(L, actions)` in `lj_ccall.c` is
  obsolete; helper comments document the fresh STOPREQ rule.

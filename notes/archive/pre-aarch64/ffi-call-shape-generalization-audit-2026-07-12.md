# FFI call-shape generalization audit

Status: source and design audit, 2026-07-12. No source, plan, or test changes
were made as part of this audit.

## Result

Commit `830297de` already removed the explicit ordinary-FFI call-shape
architecture from production source. The deleted machinery included the
per-signature `crec_call_jit_*` matchers, `lj_ccall_jit_*` C wrappers,
signature constants, declarations, and fixed IR-call entries. Production now
has one ABI-driven scalar `IR_CALLXS` recording seam instead of a growing
matrix of C prototypes.

The old catalogs remain useful only as historical ABI-coverage records. They
must not be restored or extended. The current design and remaining work are in
[Generic traced FFI calls on x86-64](generic-traced-ffi-calls-2026-07-10.md).

## Current safety gate

The generic scalar recorder and x64 `CALLXS` lowering are present, but ordinary
FFI recording deliberately aborts before emitting the call. Therefore ordinary
FFI calls currently remain interpreted. This gate is required until the
`IR_XSAVE` root materialization, sequence-valid per-TG native-call frame,
non-retiring root synchronization, exact trace/mcode lifetime pin, callback
suspension, unwind cleanup, and post-call exit protocol are complete.

Removing the gate by itself would allow remote GC to miss register-only trace
roots, permit trace machine code to be retired beneath a foreign return PC,
and leave callbacks and post-call side exits unsafe. Generalization is thus
mechanically complete, while safe activation of the generic path remains P0.

## Callback P0 findings

Two callback admission/lifetime issues are independent prerequisites for the
full natural, nonblocking FFI requirement:

1. **Cross-universe callback entry.** `callback_auto_attach()` in
   `src/lj_ccallback.c` only attaches
   when the OS thread has no current TG and rejects an already-bound TG from a
   different universe. Natural A-to-B and nested A-to-B-to-C callback chains
   therefore fail. A durable callback frame needs an exact TLS-binding swap
   guard, with exactly-once restoration on normal return and unwind.
2. **Concurrent TLS-less calls to one callback can wait on a peer.** The hidden
   callback-carrier path assigns one carrier and waits through
   `lj_threading_attach_wait()` in `src/lib_threading.c` when another invocation
   owns it. Callback entry is not an explicitly blocking Lua API, so this
   violates the nonblocking requirement. The callback slot should identify the
   callback and universe while each entrant leases or creates an independent
   carrier; a CAS loser must use another carrier rather than wait for the
   current owner.

These issues must be fixed with exact universe admission and TG lifetime
ownership. Neither issue justifies reintroducing explicit C-call shapes.

## Archive status

- [FFI C-call native-state helpers](ffi-ccall-native-helpers.md) retains a
  current description of the compact interpreted native-state protocol, but
  its explicit wrapper catalog is historical.
- [FFI C-call JIT trampoline](ffi-ccall-jit-trampoline.md) is an entirely
  historical record of the superseded signature-matrix implementation.
- The individual per-shape notes are intentionally left unchanged as an
  archive; they are not descriptions of current production source.

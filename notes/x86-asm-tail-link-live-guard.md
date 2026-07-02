# x86 assembler tail-link live guard

`asm_tail_fixup()` emitted the final trace tail jump by loading the linked
trace mcode through `trace_mcode_acq(traceref(as->J, lnk))`. Normal assembly
runs under the JIT token, but the unchecked helper composition still assumed
the target trace slot could never be stale or scoped-retiring.

The x86/x64 assembler now validates the non-self link target before using its
mcode:

- the trace slot must exist,
- `TRACE->traceno` must still match the link number,
- `TRACE->retire_epoch` must be zero, and
- the acquired mcode pointer must be non-NULL.

If the target is not live, the trace tail falls back to the interpreter target.
This keeps the compiler path non-blocking and avoids turning a stale link into
a crash.

2026-06-20

Slice: x86/shared assembler reads of already-published trace metadata.

Changes:
- Added trace_topslot_acq() for GCtrace.topslot readers.
- asm_head_side() now acquire-loads the parent trace IR base, topslot, and
  spadjust before inheriting stack-check state for a side trace.
- asm_tail_fixup() now acquire-loads linked trace mcode when the x86/x64 tail
  jump targets a different published trace. Self-links keep using the current
  trace's private mcode pointer.

Intentionally left raw:
- as->T snapshot/snapmap/nsnap/nins/topslot/spadjust writes and reads that are
  part of assembling the current unpublished trace.
- x86 exitstub/exittab setup for the current trace while mcode is being built.

Validation:
- make -C src -j$(getconf _NPROCESSORS_ONLN)
- tools/ci/m6_jit.sh

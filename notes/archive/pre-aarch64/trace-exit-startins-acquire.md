2026-06-20

Slice: trace-exit read of a linked trace's original bytecode instruction.

Changes:
- lj_trace_exit() now uses trace_startins_acq() when handling a BC_JLOOP exit
  that needs the linked trace's original start instruction.
- Removed the local retpc pointer that took the address of another trace's
  startins field when only the instruction value was needed.

Intentionally left raw:
- asm_tail_link() still uses &trace->startins as a pseudo-bytecode PC for
  generated interpreter-exit code. That is an address/lifetime design issue,
  not just a value load, so it needs a separate pass.

Validation:
- make -C src -j$(getconf _NPROCESSORS_ONLN)
- tools/ci/m6_jit_token.sh

# Profile Timer Abort Atomic

The pthread profiler timer thread and the stopping thread shared
`ProfileState.abort`, but the flag was a plain `int` with raw reads and writes.
The profile mutex protects `g->hookmask`/dispatch updates; it does not cover the
timer-thread shutdown flag.

`ProfileState.abort` is now a `uint32_t` and the timer boundary uses
`la_store32_rel()` from start/stop and `la_load32_acq()` in the timer loop. This
keeps profile shutdown in the repository's shared-memory model without changing
the existing global hook/profiler design.

Coverage model: profiler start/stop behavior and STOPREQ freshness are covered
by behavior fixtures. The profiler abort flag ownership is documented beside the
helper/code path; raw field spelling is not a test contract.

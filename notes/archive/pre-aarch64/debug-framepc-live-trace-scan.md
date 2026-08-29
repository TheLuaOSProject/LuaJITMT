# debug_framepc live trace scan

`debug_framepc()` repairs PCs that point at a trace `startins` pseudo-bytecode
after `lj_trace_exit()`. The old path derived `GCtrace *` from
`ins - 1 == &T->startins` and immediately read `TRACE->startpc`.

That trusted a raw address from cframe state. If the trace had been flushed or
retired, debug introspection could read metadata from a stale trace body.

The repair path now scans the current RCU trace vector and only accepts the
derived pointer if a live slot points to it:

- `cur == T`,
- `TRACE->traceno` matches the vector index,
- `TRACE->retire_epoch == 0`,
- `TRACE->startpt` is the inspected prototype, and
- `ins == &TRACE->startins + 1`.

Only then does it read `TRACE->startpc`; otherwise the debug location is treated
as unavailable.

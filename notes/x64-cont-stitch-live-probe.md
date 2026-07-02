# x64 cont_stitch live probe

The x64 trace-stitch continuation saved the previous `GCtrace *` in the
continuation frame and later read `TRACE->traceno` and `TRACE->link` directly.
If another thread flushed or scoped-retired that trace before the continuation
ran, the VM could consume stale trace metadata from a raw saved pointer.

`cont_stitch` now calls `lj_trace_stitch_probe(J, T)` before using the saved
trace. The helper reloads the current trace-vector slot by trace number and
requires:

- the saved pointer is non-NULL,
- `TRACE->traceno` is nonzero,
- `traceref(J, traceno) == T`,
- `TRACE->retire_epoch == 0`, and
- `TRACE->link` is not the self-link blacklist marker.

The helper returns the packed `{link, traceno}` pair only for live traces. The
VM decodes that pair and either jumps to the linked trace through the already
guarded `BC_JLOOP` path or asks C to stitch a new trace. Invalid, retired, or
blacklisted saved traces continue through `cont_nop`. The VM saves and restores
`BASE` through `L->base` around the helper call because x64 keeps `BASE` in a
caller-save register.

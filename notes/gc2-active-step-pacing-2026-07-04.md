# GC2 Active Automatic Step Pacing

Automatic allocation-triggered GC steps now stop after one GC state-machine step
while a GC2 cycle is active.  The legacy threshold is then republished at the
GC2 helper quantum and classic debt is cleared for that automatic path.

This keeps stock API semantics intact because `collectgarbage("step")` still
uses `lj_gc_step_explicit()` and retains classic debt accounting.  The change is
only for VM/JIT allocation checks, where carrying classic single-thread debt
made traced allocation loops spend an entire `stepmul` budget draining GC2 work
on the mutator.

The motivating repro was a traced unique-string allocation loop.  Before this
change, 40k unique strings ran about 215 us/string and performed 693 worker
drain batches with GC enabled.  After the change, the same probe runs about
275 ns/string and performs 47 worker drain batches.  `collectgarbage("stop")`
is no longer required to avoid the cliff.

The M9 trace hard-assist cadence test now includes a worker-drain bound for this
case.  The stock benchmark guard also uses a larger default scale so the
new-key and closure probes compare steady throughput instead of short-run timer
and setup noise.

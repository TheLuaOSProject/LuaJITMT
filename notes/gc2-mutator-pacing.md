# GC2 Mutator Pacing Batch Gate

Allocation-triggered GC2 cycles still publish the legacy threshold so existing
LuaJIT GC-step semantics continue to make progress, including stock guards that
expect allocation bursts to trigger collection.  The mutator no longer drains
that debt at the legacy 1 KiB threshold cadence once a GC2 cycle is active.

The current bridge uses `LJ_GC2_HELPER_IDLE_STEP` as the automatic republish
quantum after a bounded `lj_gc_step()` does not finish a cycle.  This keeps GC
progress visible without making allocation-heavy loops run the collector on
nearly every object.

GC2 hard assists are also batch-gated.  Normal hard limits only run an assist
when the current TG has accumulated a full `LJ_GC2_ACCT_FLUSH` local allocation
batch.  Forced tiny hard limits used by focused tests still bypass the gate so
the hard-check path remains directly testable.

Focused effect on Linux/x64, `n=100000` closure loop and `n=80000` hash-write
loop:

- `closures_upval` with GC enabled dropped from roughly 3090 ns/op to roughly
  160 ns/op.
- `tab_hash_write` with GC enabled dropped from roughly 663 ns/op to roughly
  96 ns/op in the JIT microbench and roughly 87 ns/op in the interpreter
  microbench.

This is still a bridge.  Full root-list removal and bitmap-only sweep are still
required to get the remaining closure and table allocation costs close to stock.

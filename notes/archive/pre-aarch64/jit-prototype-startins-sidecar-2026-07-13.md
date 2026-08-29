# Immutable JIT opcode recovery sidecar

JIT root traces replace several bytecode instructions with `JLOOP`, `JFORL`,
or `JITERL`. Recovering the original instruction by walking `TraceVec`, live
trace bodies, and retired trace metadata is unsafe while the exclusive retire
owner can free those structures. Waiting for its SMR writer is also not an
acceptable fallback for the cooperative GC2/JIT boundary.

Every `GCproto` now owns a fixed, zero-initialized `BCIns` sidecar immediately
after its executable bytecode. Before trace publication patches a bytecode
slot, it release-publishes the immutable original instruction to the matching
sidecar slot. Recovery first walks only the current state-owned Lua frames and
their rooted prototypes. It can therefore interpret a denied or stale LOOP,
RET, ITERN, JFORL, or JITERL without waiting for global SMR or reading retired
trace metadata. A temporary SMR collision returns a retry sentinel; the x64 VM
retries the exact recovery point without repeating an already-applied numeric
or iterator update.

The former x64 `ISNEXT` failure path no longer reads `J->tracev` or a trace body
without an entry/lifetime proof. It recovers the original ITERN from the rooted
prototype, changes only its opcode to ITERC, publishes it, and redispatches the
same instruction.

Parser and bytecode-loader constructors allocate and initialize the doubled
bytecode extent. Both GC2 prototype validators require the exact sidecar
pointer and complete extent. `sizept` remains the destructor size, and bytecode
dumps continue to serialize only the executable bytecode. A focused fixture
checks parser and dump/load geometry, immutable publication, RET/LOOP recovery,
forced local retry, and ISNEXT/ITERN despecialization results.

This dense layout is a deliberate b1.2.0 correctness tradeoff. Measurements on
the candidate showed about 19.6% more retained prototype memory for tiny
functions, 44.8% for medium functions, 50.2% for large functions, and roughly
18.3% on a synthetic build/load/introspection workload. b1.2.1 must replace it
with a lazy or sparse immutable recovery map without weakening the nonwaiting
proof.

Focused validation passed `m6_jit_token`, the sidecar fixture 20 consecutive
runs, `m5_bcdump_compat`, the 509-test JIT stock suite, the 387-test `-joff`
stock suite, and `m6_jit_flush_gc_current_stack`. The standalone stock
`misc/gc_trace.lua` oracle still fails at its pre-collection trace-number
expectation, but a clean detached build of exact parent `bcd07317` fails with
the identical 102 live slots before and after collection. This pre-existing
trace-retirement defect remains a b1.2 release issue; it is not attributed to
the sidecar layout.

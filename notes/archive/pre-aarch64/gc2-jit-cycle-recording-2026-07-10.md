# GC2 cycle/JIT recording regression, 2026-07-10

The GC2-only collector transition accidentally combined two conservative JIT
gates into a global JIT shutdown:

- `trace_current_proto_nojitroot()` rejected every prototype with local-cell
  bytecode or upvalues. The lockless parser emits `CGET`/`CSET` for ordinary
  mutable locals, so a normal numeric loop never reached the recorder.
- `gc2_mark_begin()` synchronously flushed all traces through the recorder
  token before publishing MARK. Trace compilation itself allocates and can
  request a cycle, making this a recorder-abort loop as well as a token wait on
  the collector path.

Observed at `edddec97` before the fix:

- a fresh `hotloop=1` numeric loop left `jit.util.traceinfo(1)` nil;
- `m3_gc2_scaffold` failed in `test_jit_table_store_helper_barrier()` because
  `find_trace(g)` returned nil;
- `m6_jit` failed its first redispatch fixture for the same missing trace;
- the canonical `m9_bench_stock_compare` first row measured `arith_loop` at
  6.22 ns/op versus stock's 0.43 ns/op (14.47x).

The broad prototype gate was removed. Synthetic/internal prototypes cannot be
identified safely by the rejected bytecode traits: built-in Lua helpers can use
the same function-header and local-cell forms as ordinary code. Normal
local-cell and upvalue prototypes again use their existing CGET/CSET/UREF
recording paths, while the recorder's normal opcode support remains the precise
admission rule. GC2 cycle start no longer flushes or waits for the recorder
token: live published traces are reached through prototypes, trace links and TG
vmstates, while the GC2 JIT root snapshot preserves in-flight and retired
metadata. The cycle's `HS_EXIT_TRACES` handshake plus the non-IDLE JLOOP gate
quiesce executable trace users until the cycle completes. The temporary
all-trace-slot root loop was also removed: trace-vector slots are intentionally
not semantic roots, or dead prototype/trace cycles would remain live forever.

The same transition had also inserted unconditional bailouts in
`lj_record_call()` and `lj_record_tailcall()` for every Lua callee. Those gates
made ordinary inlining, tail calls, recursion, and any non-zero snapshot frame
base impossible even for prototypes without local cells. They are removed too:
the existing call specialization, frame snapshots, RETF lowering, and the
targeted active-MT side-of-side cell rule now decide what is recordable. M6
asserts a deep inlined call chain and an ordinary Lua tail call, while the XSAVE
fixture exercises an inlined non-zero frame and an XSAVE-before-RETF trace.

MARK/WEAK still conservatively reject final recorder publication until the
complete concurrent recorder-root proof is in place. That rejection is a
transient phase collision, not a blacklist or optimization penalty: the root
hotcount is now reset to a short bounded retry. Without this distinction, a
secondary TG doing allocation-heavy work could consume every hot event during
several peer-owned GC2 cycles and remain interpreted after GC2 returned to
IDLE. `t-jit-secondary.lua` covers repeated flushes followed by an allocating
loop on a secondary TG.

Existing M3 trace-root/barrier fixtures and the M6 JIT aggregate are the
behavioral guards; the benchmark gate catches a future broad recorder shutdown.

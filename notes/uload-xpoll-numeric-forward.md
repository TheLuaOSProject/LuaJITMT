# Numeric ULOAD forwarding across XPOLL

`IR_XPOLL` remains a hard optimizer boundary for `USTORE` DSE and for
barrier-sensitive `ULOAD`s. Numeric and primitive `ULOAD`s can now forward
across `XPOLL` while `mt_active` is still zero. Thread activation flushes all
existing traces before latching `mt_active`, so these pre-MT traces cannot run
after secondary Lua threads exist.

This recovers the single-thread `upval_hot` shape: the loop keeps the numeric
upvalue value as SSA across the poll and still emits the `USTORE`, so side
exits and later interpreter code observe the heap cell update. GC-valued loads,
post-MT traces, and all `XBAR` regions stay conservative.

Focused guard: `m6_jit_barrier_xpoll` keeps the GC-valued upvalue barrier
ordering check and now also rejects a loop-body numeric `ULOAD` after `XPOLL`.

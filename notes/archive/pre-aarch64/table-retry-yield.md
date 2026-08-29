Table Retry Yield
=================

The generic table retry helpers no longer park for 1 ms. `lj_tab_wait_no_l()`
and `lj_tab_wait_l()` now do a short CPU pause loop and then yield the OS
thread through `lj_thr_yield()`.

This targets the warm retry paths that can observe transient `KEYLOCK`,
`FORWARD`, publishing claims, and retired array/hash generations. A reader that
races a short publication window should no longer pay millisecond-scale latency.

The same-table structural owner wait now uses the same retry-yield discipline.
It remains a cold bridge for compound resize/library shifts, not a warm table
lookup/store path. Same-table serialization is documented with the structural
owner note; the generic retry helper is reserved for transient sentinels and
publication races.

While testing this change, `m5_tab_resize_stress` exposed a separate GC2
invariant: a safepoint taken at native-leave can scan a stack whose VM/JIT frame
chain is still transient. `lj_native_leave()` now acknowledges a pending
safepoint before clearing the last native depth, and GC2 treats native-current
thread stacks like remote-current stacks by scanning conservatively to `maxstack`
instead of walking frame metadata.

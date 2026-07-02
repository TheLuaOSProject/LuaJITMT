Table Retry Yield
=================

The generic table retry helpers no longer park for 1 ms. `lj_tab_wait_no_l()`
and `lj_tab_wait_l()` now do a short CPU pause loop and then yield the OS
thread through `lj_thr_yield()`.

This targets the warm retry paths that can observe transient `KEYLOCK`,
`FORWARD`, publishing claims, and retired array/hash generations. A reader that
races a short publication window should no longer pay millisecond-scale latency.

The same-table structural owner wait remains a futex-backed resize park. It is a
different bridge: it sleeps on the table owner word and receives explicit wakeups
from `lj_tab_struct_leave()`, so it is not the unkeyed global retry helper this
change is removing from reader/publisher loops.

While testing this change, `m5_tab_resize_stress` exposed a separate GC2
invariant: a safepoint taken at native-leave can scan a stack whose VM/JIT frame
chain is still transient. `lj_native_leave()` now acknowledges a pending
safepoint before clearing the last native depth, and GC2 treats native-current
thread stacks like remote-current stacks by scanning conservatively to `maxstack`
instead of walking frame metadata.

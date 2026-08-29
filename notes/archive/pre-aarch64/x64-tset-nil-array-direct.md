# x64 nil array TSET direct path

`BC_TSETV` and `BC_TSETB` previously sent every in-bounds array store whose old
slot value was nil through `lj_tab_storetv_forvm_array()`. That is required when
`__newindex` may run, but it is unnecessary for tables with no metatable.

The nil-slot branch now jumps back to the existing x64 direct-store gate when
the parent table has no metatable. That gate still rejects active GC2 marking,
weak tables, active MT, metatables, and retiring separated arrays before doing a
raw single-thread store, and the existing black-table barrier check remains
after the store.

The helper path remains in force for all metatable cases, including cached
`nomm` misses for `__newindex`, because the current direct-store gate
intentionally rejects all metatables.

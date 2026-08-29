# x64 TSET Entering Helper Route

Date: 2026-07-03

The x64 `TSETV`, `TSETS`/`GSET`, `TSETB`, and `TSETR` direct-store predicate
now treats `global_State.mt_entering` as active MT. A thread that is attaching
has raised `mt_entering` before `mt_active` is visible, so the VM must not use
the direct array/hash store path during that window. The parent-aware helpers
already revalidate current array/hash generations, preserve `FORWARD` sentinels,
and run the publication barriers expected by the table-store protocol.

Coverage: `m5_x64_tset_nil_snapshot` builds the x64 TSET fixture with
`LJ_TAB_TEST_HELPERS`, executes real interpreter `TSETB`, `TSETV`, `TSETR`, and
`TSETS` bytecode while `mt_entering` is nonzero, and asserts the VM array/hash
helper counters advance. The same fixture still covers forwarded-array reroutes
for `TSET*` and real-bytecode `TSETM`.

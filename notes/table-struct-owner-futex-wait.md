# Table Structural Owner Futex Wait

`tab_struct_owner_wait()` now parks same-table structural contenders on
`GCtab.struct_owner` with the existing futex wait/wake pair instead of falling
back to generic retry-yield polling. The wait is Lua-aware: it enters native
state around the timed futex wait, polls safepoints on leave, and raises a
fresh STOPREQ before retrying the owner CAS.

The generic table retry helper remains for transient `KEYLOCK`, `FORWARD`, and
generation-publication races. This change only fixes the cold same-table
structural owner path whose release side already issued a futex wake.

`m5_tab_struct_owner` owns the behavior coverage: same-table structural
contention must park through the futex/native/STOPREQ-aware wait and the
threaded C fixture verifies the observable wait path. The helper comments own
the implementation rule; CI must not source-text check the helper spelling.

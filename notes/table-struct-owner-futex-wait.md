# Table Structural Owner Futex Wait

`tab_struct_owner_wait()` now parks same-table structural contenders on
`GCtab.struct_owner` with the existing futex wait/wake pair instead of falling
back to generic retry-yield polling. The wait is Lua-aware: it enters native
state around the timed futex wait, polls safepoints on leave, and raises a
fresh STOPREQ before retrying the owner CAS.

The generic table retry helper remains for transient `KEYLOCK`, `FORWARD`, and
generation-publication races. This change only fixes the cold same-table
structural owner path whose release side already issued a futex wake.

Guard: `m5_tab_struct_owner` now source-checks the futex/native/STOPREQ shape
before running the existing threaded C fixture.

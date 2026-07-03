# Table Structural Owner Futex Wait

`tab_struct_owner_wait()` parks same-table structural contenders on
`GCtab.struct_owner` with the existing futex wait/wake pair instead of falling
back to generic retry-yield polling. When a Lua stack is available, the wait
enters native state around the timed futex wait, polls safepoints on leave, and
raises a fresh STOPREQ before retrying the owner CAS. VM/internal callers that
only have TLS thread ownership use the same futex wait and native accounting,
but do not poll STOPREQ because there is no Lua stack to raise through.

The generic table retry helper remains for transient `KEYLOCK`, `FORWARD`, and
generation-publication races. This change only fixes the cold same-table
structural owner path whose release side already issued a futex wake.

`m5_tab_struct_owner` owns the behavior coverage: same-table structural
contention must park through the futex/native/STOPREQ-aware wait when a Lua
state is present, and through the futex/native/no-poll path when only TLS
ownership is present. The threaded C fixture verifies the latter with a
test-only runtime counter rather than an implementation detail oracle.

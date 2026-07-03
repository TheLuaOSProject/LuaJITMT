# Table Structural Owner Retry Yield

`GCtab.struct_owner` is a per-table owner token for resize publication and
compound table-library structural mutation. Same-table contenders must wait for
that token, but the wait must not be a fixed sleep or an implementation-spelling
gate. `tab_struct_owner_wait()` records test-only wait counters and then uses
the shared table retry-yield helpers.

When a Lua stack is available, `lj_tab_wait_l()` enters native state for the
short yield and polls safepoints on leave, so STOPREQ remains observable before
the contender retries the owner CAS. VM/internal callers without a Lua stack use
`lj_tab_wait_no_l()`, which still reports native progress through TLS but cannot
raise through a stack it does not own.

The release side uses only a release-store of owner zero. Waiters acquire-load
the owner word on the next loop iteration; no timed futex park/wake pair is part
of the structural-owner protocol. This keeps the path aligned with transient
`KEYLOCK`, `FORWARD`, and generation-publication retry discipline while still
documenting why same-table resize/compound mutation is serialized until the
cooperative per-generation resize/helper-copy target replaces the token.

`m5_tab_struct_owner` owns the behavior coverage: independent tables must not
serialize with each other, same-table structural contenders must wait until the
owner is released, and both L-aware and TLS-only callers must make retry
progress without making implementation spelling a test contract.

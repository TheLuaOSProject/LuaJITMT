# Huge shrink allocation failure during overflow unwinding

This diagnostic uses the immutable assertion and ASan builds from
`4423c15f24688cbeafcc498e2cb9370b21cea950`. The subsequent `c04d67b40` change
only repairs free-bin rebuild interval handling; huge realloc and stack
relimiting remain unchanged. No runtime fix is included in this diagnostic.

Both builds run eight focused raw-shrink cases. The test first proves that the
existing allocation-failure hook is active. Seven positive shrinks then return
NULL: a table-backed allocation with a retained reader within one mapping
class; table-backed cross-mapping and huge-to-small shrinks with and without a
reader; and direct cross-mapping and huge-to-small shrinks. Each refusal
preserves the complete old payload. Table-backed cases preserve the coherent
slot size, flags and reader count, then release and free normally. The unheld
same-mapping control returns the original pointer without an allocation attempt.
Returning NULL for a valid positive shrink violates the allocator's no-failure
shrink requirement even when the old storage remains intact.

The embedding witness also reaches a real stack overflow through ordinary Lua
recursion inside `lua_pcall`. Its C error handler observes the emergency stack
and optionally acquires a real HugeTab reader. It does not fabricate stack
metadata. With allocation enabled, both assertion and ASan builds return
`LUA_ERRRUN` and clean up normally, with or without that reader.

The unwind relimits a 524,400-byte emergency stack to 524,072 bytes. This crosses
a physical mapping boundary from 589,824 to 524,288 bytes, so the current huge
allocator allocates and moves the stack. A retained reader keeps the original
mapping readable until its release.

A separate variant enables allocation refusal immediately before the error
handler returns. All four runs, assertion/ASan with and without a reader,
reach that injection point and fail to return from protected unwind within the
eight-second watchdog. They are recorded as exit 124, with empty stderr and
core dumps disabled. These are failing baseline observations, not passing
stability checks. A timeout alone does not prove infinite execution.

A bounded debugger run stops at the first `lj_err_mem` call. Its stack shows
`lj_mem_realloc` called by `resizestack` inside `err_unwind`, while the original
stack-overflow exception is already being unwound. Exactly one injected
allocation refusal has occurred; the stack remains 65,550 slots, and the
retained reader still covers all 524,400 old bytes. This directly connects the
secondary allocation error to stack shrinking during error unwinding.

The next implementation must let a valid raw huge shrink retain its original
mapping and return without allocation, waiting for readers or moving the
active stack. Subsequent growth/free must identify physical allocation class
independently of the now-smaller logical size, preserve authoritative physical
extent and copy only the caller's logical prefix. Typed entries and existing
growth/free arbitration need their own unchanged ownership checks. Retaining
physical capacity also needs explicit memory controls; it is not a memory-parity
claim.

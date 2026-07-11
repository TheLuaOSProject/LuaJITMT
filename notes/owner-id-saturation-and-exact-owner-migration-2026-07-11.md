# Owner-ID saturation and exact-owner migration (2026-07-11)

## Runtime defect closed

`lj_thr_newid()` formerly used wrapping 32-bit fetch-add. At the top of the
range it could first return `LJ_THREAD_GCSCAN` (`0xffffffff`), which is reserved
for collector stack claims, and then wrap to an already-issued owner ID. Either
case destroys the meaning of `lua_State.owner`, TG/arena ownership, handshake
leadership, and JIT-token ownership.

Owner-ID allocation now uses a relaxed CAS loop and permanently saturates after
issuing `0xfffffffe`. It returns zero on exhaustion. Zero is already the
ownerless/unclaimed value, so every production allocation site must treat it as
admission failure rather than install it in a TG or state:

- `lua_newstate()` returns `NULL`;
- OS-thread creation returns `EAGAIN`;
- `threading.spawn()` raises an ordinary runtime error;
- foreign-state attach returns false;
- GC2 worker-pool growth fails and unwinds the partial pool.

IDs consumed by later allocation or OS-thread-creation failures are deliberately
not recycled. Recycling without an exact lifetime proof would reintroduce the
same ABA bug. The practical capacity remains 4,294,967,294 process-lifetime
owner-ID reservations/attempts, including attempts whose later allocation,
claim, or OS-thread creation fails.

## Why this is not the final owner identity

Saturation prevents silent corruption, but a fully migrated runtime should not
use a bare 32-bit ID as proof that a particular TG body remains alive. Stable TG
registry slots already carry a non-reused incarnation and an exact borrow token.
Long-lived owner routes must eventually carry that exact key/borrow (or execute
inside an operation-scoped borrow) before legacy TG-list exclusion can be
removed. This includes state hints, arena owners, JIT/FFI owner publications,
worker slots, SSB holders, and raw owner-lookup results.

Until that migration is complete, saturation is the conservative compatibility
boundary: no reuse, no sentinel collision, and explicit failure instead of a
false ownership grant.

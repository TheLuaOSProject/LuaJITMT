# String Table ID And Count Blocks

`GCstr.sid` is a uniqueness discriminator used by table string-key hashing.
It does not need to be dense. Duplicate-intern losers already allocate a string
object, consume an ID, lose the bucket CAS, and free the unpublished object, so
the existing semantics already allow gaps.

The string allocator reserves a small ID range per `TGState` and assigns IDs
from that local range before touching `g->str.id` again. This removes a global
atomic cache-line hit from most successful string allocations while preserving
atomic uniqueness through the range-reservation fetch-add. Unused IDs in a
terminating thread group become harmless gaps.

`g->str.num` remains the shared resize/shrink and close-time accounting field,
but it no longer needs a global RMW for every successful intern. A successful
bucket CAS consumes one `TGState.strnum_credit`; when the local credit is empty
the TG reserves a small count block from `g->str.num`. That makes the published
count conservative while credits are outstanding, which can grow the table
early but cannot delay a required grow or shrink below reserved capacity.
Unused credits are flushed on TG detach/fini and before the close-time leak
assertion, so final accounting is exact.

Duplicate-intern CAS losers do not consume count credit because they did not
publish a string into the table. Freeing a real interned string still subtracts
one from `g->str.num`; the later credit flush subtracts only the unused
reservation. This preserves the existing free/close invariant while removing
the shared-counter write from the hot successful-intern path after refill.

Coverage is runtime-based: `m5_strtab_cas` creates unique strings, checks their
live `sid` values for uniqueness, verifies duplicate interning still returns
the original object, confirms the ID and count reserve paths refill far less
often than one global operation per string allocation, and verifies flushing
unused count credits restores `g->str.num` to the exact live-string count. The
helper comments carry the ownership rationale for the shared counters.

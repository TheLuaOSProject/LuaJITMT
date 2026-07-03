# String Table ID Blocks

`GCstr.sid` is a uniqueness discriminator used by table string-key hashing.
It does not need to be dense. Duplicate-intern losers already allocate a string
object, consume an ID, lose the bucket CAS, and free the unpublished object, so
the existing semantics already allow gaps.

The string allocator reserves a small ID range per `TGState` and assigns IDs
from that local range before touching `g->str.id` again. This removes a global
atomic cache-line hit from most successful string allocations while preserving
atomic uniqueness through the range-reservation fetch-add. Unused IDs in a
terminating thread group become harmless gaps.

`g->str.num` remains a successful-intern counter because resize accounting
needs the number of strings actually published into the current table. The ID
reserve does not change that accounting or the resize/rehash protocol.

Coverage is runtime-based: `m5_strtab_cas` creates unique strings, checks their
live `sid` values for uniqueness, verifies duplicate interning still returns
the original object, and confirms the reserve path refills far less often than
one global operation per string allocation. The test does not inspect source
text, helper names, generated IR/ASM, objdump output, or mcode bytes.

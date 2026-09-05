# Huge mapping tail for overflow authority: design audit, 2026-09-05

This records an **unimplemented design at the audit boundary**. No runtime
source was changed and no new build, runtime test, or timing comparison was
performed for it. Any later implementation requires separate evidence.

The reviewed dense-overflow prototype, based on d680421c, allocates a separate
16-byte wide proof for each traversable Huge mapping. A fixed mapping tail
could remove that additional `calloc`/`free` while preserving `GCAhdr = 128`,
the payload address, and the existing header pointer. The exact reviewed
source hashes, dense patch, full audit, and arithmetic model are in the
[evidence directory](evidence/gc-huge-tail-overflow-audit-2026-09-05).
`artifact-manifest.json` covers those artifacts and this note.

The narrow proposal preserves the size-only mapping API by reserving 16 bytes
for every Huge mapping. Plain mappings keep a NULL proof pointer. With
`A = 65536`, `H = 128`, and `W = 16`:

```
mapping_bytes = round_up(logical_size + H + W, A)
payload       = mapping_base + H
wide_proof    = mapping_base + mapping_bytes - W
```

Addition, rounding, and address-space checks must precede allocation.
HugeTab's advertised size remains the original logical size, so padding and
the proof cannot become readable payload. Proof initialization and pointer
publication must precede the HugeTab locator. Fresh anonymous zero mappings
can establish initial zero contents without touching the tail, if that
invariant is retained explicitly.

The proof stays at the physical mapping tail across same-extent realloc.
Placing it immediately after the logical payload would let growth overwrite
an old proof location. Both realloc comparisons and every map/unmap path must
use the same extended size formula. Remove both separate proof frees, retain
the existing reader/token/descriptor grace, and preserve authoritative size
snapshots through deferred free, transfer, failed insertion, and terminal
teardown. The direct realloc path must not acquire an unsafe header probe to
recover the old mapping kind.

There are costs to measure before selecting this layout. For logical sizes
65,393 through 65,408 bytes, reserving the tail grows the mapping from 64 to
128 KiB. That boundary occupies 16 byte residues per 64 KiB period; it is not
a measured workload frequency. For a 20,000-byte payload, explicitly touching
the tail can fault in an otherwise unused 4 KiB page. Deferring that touch
until promotion avoids the initial fault but does not remove its later cost.
Removing a heap call alone therefore does not establish a memory or speed win.

The full audit inventories mapped versus advertised bounds, containing
lookup, both realloc routes, deferred free, transfer, unmap, errno, and OOM
behavior. Proposed controls include boundary payload writes, padding/proof
admission rejection, same-extent and moving realloc, paused scanners,
protected-proof header-only token completion, and exact whole-mapping unmap
counts. These are validation requirements for a future implementation, not
tests reported as passed here. Removing the extra heap allocation would not
make mmap, page faults, or the whole collector nonblocking.

Subsequent source review confirmed that `hugetab_claim_realloc` rejects every
published `TRAVERSABLE` identity. A promoted Huge table with a retained reader
must therefore demonstrate realloc refusal. Accepted same-extent or moving
traversable realloc belongs only to the private, caller-owned direct path;
published plain Huge storage keeps its existing allocate/copy/defer contract.
Future controls must preserve those gates. The historical full audit and
arithmetic model remain unchanged; they do not report successful published
traversable realloc.

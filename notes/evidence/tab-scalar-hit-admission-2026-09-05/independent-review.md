# Independent scalar-hit source review, 2026-09-05

No correctness blocker found in the five production files against
`d680421c4cb50b85437d88255bc89358c5e3a6b1`. This was a read-only source audit,
not an independent rerun of the owner's test or performance results. The
separate pending arena empty/reclaimed changes were excluded.

All five shared production files matched the frozen candidate exactly at
review time:

| File | Git blob |
| --- | --- |
| src/lj_gc2.c | 8cfdceba49e1d01e86a5e5ec8dae885b484ba9de |
| src/lj_gc2.h | cc3129bf9344559dc88fbf4120ad3195d8adc070 |
| src/lj_meta.c | f99d24ee9a01eb90addfeef37cbfa31030c1c7da |
| src/lj_tab.c | 2936c295abc8ef1581eac4992d8eec0aa23d9553 |
| src/lj_tab.h | 902e6f19215cb4b64c7fc6f7734368acc369c9f6 |

The reviewed arena blobs are d680's `9686ba18d5f3723a447a0ca4a7b3b9dc810729dd`
and `3cd3302ae3fa4c14708d092ecbd5caee9f77127d`. Complete candidate SHA-256
identities are in
`/tmp/lj-tab-scalar-hit-20260905-mosheh9q/validation-snapshot.json` and
`bench/tab-scalar-hit-2026-09-05/source-snapshot.json`.

The following checks support the conclusion:

- `lj_gc2_small_lease_try` classifies a numerical candidate before reading a
  header. The persistent registry's exact slot reader protects the mapping
  until `remote_active` admission succeeds. The SC fence precedes the typed
  lifetime and metadata checks, matching the established writer handshake.
  Typed starts must be traversable, readable, block-present, ready, not cdata,
  not late and not FREEING; minimum extent is checked before `gct` access.
  Wrong type or transient denial leaves no retained public lease.
- Plain vectors require a plain arena and live exact start. The existing
  plain-arena gate refuses a held writer before payload access. A held reader
  prevents the writer from overwriting the body. The new call does not claim
  that this local gate has disappeared.
- The cell span is an upper bound, not an exact requested allocation size.
  The helper admits enough metadata-described space for a fixed header before
  reading it; the caller then confirms the actual table vector pointer and
  uses the immutable size header as the physical allocation contract. Size
  validity, span limits, arena-end limits and bounded collision-node addresses
  precede slot dereferences. A side-vector source that became a successor at
  the same address is acceptable only if the authoritative table root really
  publishes it. The new API's documented provenance precondition matters.
- Table and requested string bodies are retained before their source words
  are confirmed. Separate arrays and node vectors are retained before their
  publishing pointers are confirmed and their headers used. Colocated storage
  remains covered by the table admission; the empty string and nil node are
  globally owned. Final paired-vector, source-word and exact state-owner checks
  run before output publication while all leases remain held.
- Numeric and boolean keys need no child-body access. A string hash reads only
  the requested retained string's immutable `sid`. Other collision keys are
  opaque words. A positive hash result retains the node vector through key
  confirmation after the scalar load. Shared canonical keys are irreversible
  within that vector; shared clear/delete changes values. Private raw clearing
  cannot overlap the owner-only, callback-free production attempt. Clear and
  resurrection of the same key can therefore yield an ordinary valid scalar
  without combining an unrelated key's value.
- Number/boolean output avoids the result-slot-to-GC-body ABA transfer problem.
  No source read follows the output write, including either input alias. Stack
  publication still increments the existing dirty epoch; the scalar cannot
  enter the GC-object barrier. Every denial path releases all acquired leases
  and leaves the input/output cells unchanged.
- The production fast attempt has no allocation, Lua callback, stack growth,
  global SMR admission or explicit retry wait. Registry and reader-count CAS
  loops remain lock-free retries rather than a wait-free bound. Lease release
  can perform existing arena metadata cleanup/wake work; this is not a promise
  of a fixed instruction count. Misses and unsupported cases continue into the
  broader allocating/waiting meta path.

Caller provenance was checked beyond the new declaration. VM TGETV uses the
owned stack; TGETB uses a scalar temporary; TGETS's string temporary is backed
by the active prototype's immutable constant root. The function-environment
variant bypasses the fast path. C API gettable/getfield holds the state claim,
uses owned stack/registry/current C-closure upvalue cells, and materializes
environment pseudo-indices into a stack root through the existing capture.
The active frame keeps the current C closure container alive during the
callback-free attempt.

There is one documentation-only completeness item: `lib_ffi.c:1134` is another
caller of `lj_meta_tgettv_rooted`. Its `ffi_index_meta` implementation transfers
the metatype table from an exact `LJCTypeMetaRoot` to enumerated `base`, publishes
that stack root, releases the private root, then passes `base`, `base+1`, and
`base`. It therefore meets the same provenance and output-alias contract. The
owning agent was asked to add this caller to the note's provenance list; no
production repair is needed for it.

The original snapshot JSON deliberately predates the final fixture assertions.
The final fixture is blob `ac74d35586699d5e20ad438eaae36dfac4cbb404`, SHA-256
`3f22601cdf84eaafec855d25b82ab7fac71d7111afcf97d63b1ac04400c6df20`.
`/tmp/lj-tab-scalar-hit-20260905-mosheh9q/final-validation.json` records its
passing strict helper/assert and ASan runs after the final key/retired-queue
assertions. The broader canonical batch used the preceding fixture; the five
runtime production blobs did not change. That boundary should remain explicit.

The new tests meaningfully cover paused real IDLE metadata reclamation with
ordinary interpreted reads, bounded refusal under a paused unrelated plain
writer, authoritative-source replacement, real resize and retired-vector
drainage, collision-key opacity, protected candidate/vector pages, size
rejection, and both result aliases. The source-SMR-only negative restores the
old blocked route. They support this positive small scalar path; they do not
establish progress for Huge vectors, GC results, mutable function environments,
metamethod chains, custom allocator operation or general ordinary reads.

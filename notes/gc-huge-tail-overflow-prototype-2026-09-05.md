# Huge tail overflow-storage study, 2026-09-05

This frozen study validates an alternative to the extra Huge allocation in
the [dense overflow prototype](gc-table-dense-overflow-prototype-2026-09-05.md).
It reserves a 16-byte wide proof at the physical end of each Huge mapping.
Traversable mappings publish that pointer; plain mappings leave it NULL.
Fresh anonymous zero-filled storage initializes the proof without touching its
page. Exact logical payload bounds, copying and live-byte accounting exclude
padding and proof storage. The inline 16-byte stamp, 8-byte token, 128-byte
header and emitted allocation/reset indexes remain unchanged.

For both Huge kinds, physical extent is
`round_up(payload + 128 + 16, 65536)`, with overflow rejected before addition.
Every map/unmap/realloc route uses that geometry. The proof lives until final
mapping release and is never separately allocated or freed. Published
traversable realloc retains its existing refusal; successful fixed/moving
traversable geometry tests use private caller-owned storage. Published plain
resize retains the old extent while a reader owns it and completes deferred
handoff afterward. Header-only FREE/DEFER_FREE consumers never read the proof.

The immutable source baseline is `28de50a622e489019fa22845d6454e029b210582`
plus dense authority and tail storage, not a final combined build. Source patch
SHA256 is `dca24e13fdc47d886a1ffc07b42a604c4e430090fa5f2fd9b1f8111678a8ee4a`.
All 210 tracked production files were checked across the five positive trees;
tail/calloc variants differ only in arena storage. Later permanent regression
integration and the repaired full FNEW fixture are separate evidence.

Strict and target-only ASan/LSan checks pass geometry, full payload writes near
each mapping boundary, untouched-tail mincore, exact logical bounds, overflow
rejection, private resize, published resize gates, retained readers, map and
locator failures, transfer/fini, and exact whole-mapping unmaps. Dense authority
checks cover small/Huge promotion, paused inline/wide scanners, peer progress
on both sides of mode publication, cell reuse and pending tokens, repeated
full collection, post-store allocation denial and full namespace containment.
Full traversal protects the actual tail and later payload pages with PROT_NONE
while exercising header-only completion. Both normal variants pass stock
387 interpreter and 509 JIT tests. Runtime ASan uses leak detection without
suppressions; build generators are not included in that runtime claim.

Three bad-source controls fail causally: old mapping geometry lets the final
payload byte overwrite the proof; the old private-realloc extent comparison
misses a required move; body-proof access by a header-only consumer faults on
the protected real tail. Initial TNEW include, terminal-orphan link and mmap64
wrapper setup failures remain preserved alongside corrected passes. Invalid
allocator-identity FNEW results from the earlier experiment are not accepted.

All 380 bounded cost processes complete on CPU 31, GCC 14.2/glibc 2.41, with
4 KiB pages on a shared host. Direct costs use seven alternating pairs, 512
retained objects, five boundary sizes, both mapping kinds and three touch
patterns. Runtime userdata costs use three pairs and 256 objects per size,
GC enabled, full payload writes and explicit retained/released collections.
No GC cycle advances inside those timed allocation loops; collection is
verified separately outside timing.

Tail storage removes 512 direct or 256 runtime proof calloc/free pairs and
saves 16 KiB or 8 KiB of glibc chunks. Ordinary payload allocation changes
range from -1.5% to -0.3%; runtime userdata medians range from -1.3% to +0.4%.
Untouched traversable allocation at size 65393 regresses 10.1% at the median
(paired range -4.1% to +10.7%). Sizes 65393 and 65408 cross from 64 to 128 KiB
physical extents, adding 32 MiB virtual space for 512 objects, without doubling
RSS. Touching a proof beyond the last payload page adds approximately
1964–2044 KiB RSS and 506–507 minor faults per 512 objects; allocation rises
5.0–8.3%, and 20 KiB-object free cost rises 31.4% in that promoted case.
The ordinary 20 KiB payload-only free median falls 8.4%. A proof sharing the
last payload page has no corresponding extra-page pattern.

This removes a heap dependency and preserves continued collection through
ordinary dirty-counter rollover. It is not a uniform memory/speed win or a
complete nonblocking allocator: full 96-bit authority exhaustion still vetoes
reclamation, and mapping, faults, SMR, worker and other runtime dependencies
remain. [Functional evidence](evidence/gc-huge-tail-overflow-prototype-2026-09-05/)
and [cost evidence](../bench/gc-huge-tail-overflow-prototype-2026-09-05/)
retain all text artifacts, exact hashes, failures, commands and limits;
binary artifacts remain local with archived identities.

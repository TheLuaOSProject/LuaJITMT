# Huge overflow proof in mapping tail: bounded design audit

An aligned W at the end of the Huge mapping is a defensible way to remove the
prototype's separate 16-byte calloc/free. It can preserve GCAhdr=128, payload
start=base+128, the existing kind-discriminated pointer, and the proof lifetime.
It is not automatically cheaper in resident memory. No runtime edits, builds,
or timings were performed for this audit. Exact dense source hashes and a
boundary arithmetic model are in `source-and-geometry.json`.

The reviewed source is the frozen normal dense prototype under
`/tmp/lj-dense-overflow-20260905-7tl6kcfk`, based on d680421c. The current source
allocates W by calloc before publication at lj_arena.c:1932–1940 and frees it
in both Huge unmap paths at 1955 and 1968. Its body-proof lookup is separate
from the header-only token accessor. This audit identifies implementation
requirements and cost tradeoffs, not a failing lifetime execution in that code.

## Narrow geometry

The smallest change to the existing size-only API is to reserve a 16-byte tail
in every Huge mapping, including plain Huge mappings. Plain mappings retain
a NULL W pointer and never use the reservation. Let A=65536, H=128, W=16:

```
M(s) = round_up(s + H + W, A)
payload = mapping_base + H
wide = mapping_base + M(s) - W
advertised_size = s
```

Check the addition and rounding before mapping. For example, reject size at
or below LJ_HUGE_THRESHOLD and size greater than
`(SIZE_MAX & ~(A-1)) - H - W`. Keep arena_map_aligned's separate span/address
limits; the size-only arithmetic helper does not prove a virtual mapping is
possible. The 16-byte tail is aligned because the mapping extent is A-aligned.
The W address is derived while the allocation is private, never from an
unadmitted candidate's header or advertised size.

Initialize the complete proof before publishing the header pointer or HugeTab
locator. In this Linux allocator, arena_map_aligned obtains fresh anonymous
zero-filled pages; that establishes zero contents without a separate heap
allocator or a synchronizing initializer. The implementation could avoid
explicitly touching the tail until the first promotion if it deliberately
documents and retains that fresh-zero-mapping invariant. It must not copy a
dirty retired mapping into this path or treat an old W as zero on reincarnation.
An explicit 16-byte store is simpler to inspect but can fault in a new page.

W is at the physical mapping tail, not `align_up(payload+s,16)`. The latter
changes address during a same-mapping realloc and can put a previously issued
proof pointer inside the expanded user payload. A fixed mapping-tail address
remains unchanged whenever M(old_size)==M(new_size), and the new mapping-size
formula guarantees the expanded payload still stops before W.

## Required source updates and preserved boundaries

| Source site in frozen dense tree | Obligation |
| --- | --- |
| lj_arena.c:1910–1917, lj_arena.h:613 | Make the single mapsize formula include W with checked overflow. Document it as physical bytes, not advertised payload. |
| lj_arena.c:1919–1941 | Map M(s), retain live_cells as the mapped extent, initialize/publish a pointer to its own tail for traversable Huge only. Remove the second calloc/OOM branch. |
| lj_arena.c:1944–1971 | Both unmap functions must use the same M(s), remove free(W), preserve existing token/descriptor admission and errno behavior. An interior tail pointer must never reach free. |
| lj_arena.c:1996–2005 | HugeTab packing still stores exact s and its flags. Its size validation uses the updated helper; do not add W to the logical 32-bit size field. |
| lj_arena.c:3107–3158, 3165–3208, 3250–3277 | Ordinary/interior/CDATA lookup bounds remain `[payload,payload+s)`. Padding and W are not root candidates, readable vectors, or user bytes. |
| lj_arena.c:3664–3699 | Header-only token admission and the token pointer stay in GCAhdr. A DEFER_FREE/header-only owner must not dereference W just because W is mapped. |
| lj_arena.c:8479–8481, 8655–8663 | Both direct and HugeTab realloc comparisons use the new M(s). Keep W stationary on same-extent resize; copy only min(old logical,new logical) on replacement. |
| lj_arena.c:8582–8596 | Header/owner/global binding still precede locator insertion. Failed insertion unmaps the whole mapping once, with no standalone tail cleanup. |
| lj_arena.c:8601–8619, 8681–8698 | External/deferred free uses the authoritative HugeTab snapshot size; do not free W at logical destruction or while an old reader survives. |
| lj_arena.c:2368–2452, 4890 onward | Terminal fini and TG transfer retain their exact slot/grace protocol. Transfer moves no W storage; the pointer remains in the same mapping. Source/destination must not acquire separate ownership of it. |

All production uses of lj_arena_huge_mapsize found in this tree are in those
map/unmap, pack, and two realloc classes. Test observers in t-arena-huge,
t-arena-hugetab, and t-tg-terminal-orphan use the helper too and need boundary
expectations updated. Huge live-byte accounting uses HugeTab's logical size;
keep it separate from actual reserved/committed bytes so metadata overhead
does not become scanner payload authority.

The current direct realloc comment explicitly avoids reading a possibly stale
header before its size-only fast decision. Do not replace that decision with
an unadmitted header-kind probe merely to recover tail size. A kind-dependent
size API can avoid adding a plain-Huge tail, but is a wider change: every
unmap, authoritative free snapshot, size validator, test observer, and both
realloc decisions must carry the original immutable allocation kind. The
caller's new requested flags are not automatically the old mapping kind.
Reserving W for all Huge sizes is the narrower initial prototype.

## Failures, errno, and memory cost

Removing calloc removes a new cold allocator dependency and its separate
failure point. It does not make mmap, munmap, page faults, or the whole GC
nonblocking. Existing arena_map_aligned restores entry errno on normal mapping
results and retries. Huge unmap saves/restores errno explicitly. Preserve
those rules after removing free(W). A new tail formula can reject a size or
make a boundary mapping larger; allocation failure still returns NULL without
publishing any partial object. Failed HugeTab insertion must release the full
M(s), never the old unextended length.

The dense sidecar-allocation injection currently also simulates the Huge
calloc failure. A tail implementation has no such second allocation. Retarget
Huge failure validation to failed map/extent/pre-publication preparation and
locator insertion, and retain the real small-sidecar calloc failure test.
Do not retain a production initialization wait solely to preserve that hook.

In the arithmetic model, a tail adds a full 64 KiB mapping quantum exactly
when an old mapping has fewer than 16 padding bytes. For one size residue
period, that is 16 of 65,536 byte residues; this is not an empirical workload
frequency. For example s=65,392 still maps 65,536 bytes, while s=65,393 through
65,408 need 131,072 instead of 65,536. A careless "use existing final 16 bytes"
implementation would overwrite legitimate payload at those boundaries.

The resident cost can be more significant than those rare extra mapping
quanta. At s=20,000, the fully touched payload ends on page 4 of a 64 KiB
mapping, but W lies on page 15. Explicit initialization or later promotion
can therefore fault in another 4 KiB page for just 16 bytes of proof. The
current heap allocation can pack many W words onto one heap page. Using the
existing guaranteed zero mapping without touching W until promotion avoids
that common initial fault, though promoted objects can still pay it. Measure
reserved bytes, resident/private-dirty pages, page faults, and allocator calls
separately; a calloc-count improvement alone does not select the better design.

## Targeted controls before accepting an implementation

- For s around every `k*A-H-W` and `k*A-H` boundary, plus the Huge threshold
  and arithmetic upper bounds, require valid alignment, exact map length,
  non-overlap of full advertised payload and W, and no integer wrap.
- Write every logical byte, including the last byte, then use and verify W.
  A control with the old mapsize or W at old map end must fail at a boundary.
  Probe lookup and counted body admission at payload+s, W, and trailing
  padding; all must reject those addresses as payload.
- Exercise same-extent growth/shrink with W already promoted and an old
  retained reader; grow across the new map boundary with allocation/copy.
  The invalid old-mapsize comparison must fail before payload can reach W.
- Pause a real old scanner, publish renewal, and test huge/small proof parity.
  Keep the existing wrong-type, reader overflow, late external-free, and
  suspended reader controls. Plain Huge must keep the pointer NULL.
- Protect the W page and body pages during header-only FREE/DEFER_FREE token
  completion. It must complete without reading proof bytes. A bad control
  that reuses body-proof admission at this header-only boundary must fault.
- Count OS unmaps through failed locator insertion, replacement failure,
  deferred free, transfer and terminal fini. Require exactly one whole M(s)
  unmap after all readers/tokens release and no free(interior-W).

This tail design is implementable without changing the hot proof access
protocol. Its physical-memory behavior and its universal-versus-kind-aware
size contract should be chosen explicitly before implementation.

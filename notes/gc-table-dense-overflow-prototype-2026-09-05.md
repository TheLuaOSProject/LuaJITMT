# Reserved dense overflow authority: isolated study, 2026-09-05

The isolated dense-W prototype renews table scan authority at the 32-bit dirty
boundary while retaining the existing 16-byte inline entry and emitted VM/JIT
indexes. Its focused strict and leak-enabled ASan controls pass. In the sampled
normal workloads, ordinary public barriers cost a median 0.262% more than the
exact base; promoted barriers cost 61.569% more than dense ordinary barriers.
Reserved small metadata doubles. These are bounded prototype results, with
integration and memory costs still to assess.

The tested base is `d680421c4cb50b85437d88255bc89358c5e3a6b1`. It excludes the
later arena, scalar-read, direct-hit and Huge-tail work. The
[preserved audit and functional evidence](evidence/gc-table-dense-overflow-prototype-2026-09-05)
contain exact source/build/binary identities, fixtures, commands, negative
controls and development failures. The
[normal cost package](../bench/gc-table-dense-overflow-prototype-2026-09-05)
contains all 153 recorded processes, memory snapshots and the unchanged plan
harness. Packaging verified the recorded hashes and regenerated the summary
from those records; it did not rerun a runtime or benchmark.

`dense-W.patch` identifies the frozen tested and measured source. The separate
`dense-W-candidate.patch` adds only `accessor-comment.patch`, which documents
the small-only header-union accessor precondition. That comment is absent
from the frozen binaries. Both patch reconstructions are checked. The original
`audit.md` remains unchanged, including its historical statement that its
author had made no shared note edits when the study finished.

An ordinary mutation increments the inline dirty word and clears coverage
using CAS64. At the promotion boundary, it first invalidates an already
reserved wide proof, then publishes the inline sentinel using CAS64. Wide
proofs contain `{era64, covered_cycle32, serial32}` and use CAS128 snapshots and
updates. A low-word rollover increments the era and clears coverage atomically.
Scanners retain their captured domain: an inline scan cannot adopt wide
authority after reading payload, and a wide completion must match both era and
serial. Exact rescan-token generation and recovery ownership remain separate.

Wide authority stays monotone for the mapping/cell lifetime, including cell
reuse. C allocation, VM TNEW and emitted FNEW reset the existing inline state;
the next promotion invalidates any retained wide coverage before exposing it.
No publisher owns an initialization state that a suspended actor can leave
unfinished. Small mappings reserve the complete wide array before publication;
traversable Huge mappings reserve one 16-byte allocation. Promotion after a
payload store makes no allocation. Missing authority and full era/serial
exhaustion retain the sticky reclamation veto. Global cycle and token limits
are unchanged; this does not make the whole collector nonblocking.

| Layout | Exact d680 | Dense W |
| --- | ---: | ---: |
| Inline entry / token offset | 16 B / 8 B | 16 B / 8 B |
| Small sidecar request | 65,536 B | 131,072 B |
| Fixed Huge header | 128 B | 128 B |
| Separate traversable Huge proof | 0 B | 16 B |

The functional controls cover eight paused old scans across inline/wide,
small/Huge and legacy/exact-token combinations; four pauses around sentinel
publication with a peer completing; and twelve forced rollovers/full
collections on each mapping kind. Fresh descendants remain live, unreachable
weak values clear, recovery drains and collection reaches IDLE without a
reclamation veto. Existing coalescing, traversal, recovery and store-guard
checks pass with terminal saturation adapted to the full wide pair. Protected
wide storage proves that small FREE and Huge DEFER_FREE token completion stay
header-only. Actual TNEW/FNEW at cells 1536 and 1537 cover reuse, neighboring
guards and exact token ownership. Reservation failure remains private; denying
allocation after a store does not prevent promotion. Normal stock tests pass
387 with JIT off and 509 with JIT on for both the base and dense build.

Three deliberately broken source variants fail: letting an old inline scan
adopt wide authority, ignoring the captured era, and exposing the sentinel
before invalidating wide coverage. The first two wrongly complete an old
exact scan; the third exposes previously current coverage. Their patches,
commands and assertion failures are retained.

The inherited full FNEW capacity failure and its later settled pass are also
retained. That settled pass is diagnostic only: its allocator-identity setup
left construction lanes unfinished. The final consistent `mt_entering`
fallback fixture passes strict and ASan; the malformed-identity control fails
both. It does not directly validate the false allocator-identity gate. The
[later production fixture repair](fnew-fixture-valid-allocator-2026-09-05.md)
adds that coverage separately and is not retroactively part of this study.

The cost study used matched normal static builds, seven alternating groups
for barriers and each plan case/mode, and three fresh pairs for each retained
memory workload. CPU 31 was reserved, while other work could run on CPUs 0–15
and 30; host frequency was not isolated. The plan harness retained automatic
GC and its best-of-five rounds at `BENCH_SCALE=0.02`. Small differences do not
establish general parity. Its JIT table-allocation case permits allocation
elimination and is near reporting resolution, so retained-object memory runs
provide the separate materialized-allocation evidence.

For 20,000 retained tables with JIT off, both builds retain 41 traversable
mappings. Requested sidecar bytes increase from 2,686,976 to 5,373,952.
On this glibc, the dense 128 KiB request uses a 135,168-byte mmap, leaving most
untouched wide pages nonresident. Median current RSS after collection is
9,260 KiB for base and 9,124 KiB for dense; virtual size is 13,412 versus
16,200 KiB. This allocator-specific residency behavior is not a memory saving.
Promoting all tables adds 2,356 KiB within the dense process, versus 100 KiB
for base barriers/scans. The d680 allocator retains those mappings after roots
are released; state close releases the dense sidecars. Their requested bytes
remain outside ordinary GC live-byte accounting.

The measured ordinary workloads allocate Huge raw vectors, but no traversable
Huge wide proofs. They do not measure the additional per-Huge `calloc`/`free`
cost. The [Huge-tail design audit](gc-huge-tail-overflow-audit-2026-09-05.md)
and later isolated implementation have separate source and evidence boundaries.

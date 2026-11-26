# Infallible small plain allocation shrink

This change starts at `78f117627cdecbe83600d9e1d39c0b0f923459c1`. A valid
positive shrink of small raw storage now returns its original address without
allocating, waiting for readers, or changing their readable extent. This fixes
the small-allocation part of the allocator contract: Lua assumes shrinking an
existing allocation cannot fail. [Lua 5.1 allocator contract](https://www.lua.org/manual/5.1/manual.html#lua_Alloc).

The previous implementation needed an exclusive writer generation to publish a
reusable suffix. A reader of the same allocation retried while that writer was
suspended. An existing reader instead forced a move, which could fail for lack
of memory. A deterministic witness against the frozen baseline holds a real
4 KiB storage lease while shrinking to 17 bytes. With memory available it gets
a replacement. After exhausting the bump window and refusing `mmap64`, it gets
null. The candidate keeps the original pointer in both cases, requests no
mapping and preserves all 4 KiB of old bytes. The initial diagnostic wrapped
only `mmap`; it missed the library's `mmap64` import and is preserved separately
without being counted as an allocation-failure proof.

The new path leaves block boundaries, payload bytes, reader admission and all
semantic side owners unchanged. A start-only bit in the PLAIN arena's otherwise
unused cdata plane records that logical size is smaller than physical extent.
Typed cdata publication and discovery still require traversable storage and
their existing lifetime authority. No header, arena or sidecar layout changes.
The shrink caller must own the allocation, as with ordinary allocator calls;
concurrent reallocation or freeing of that same raw pointer is not made valid
by this change.

Local free and retained-destructor completion recover the whole physical span
under their existing writer authority. They use the next block/mark boundary
and, for the last allocation, the owner's active bump cursor. Foreign frees
cannot consult that cursor, so a flagged allocation takes the durable late-free
path and wakes its owner instead of publishing an intrusive node with a short
size. Deferred frees and failed admissions already preserve that same intent.
Full-span ownership preflight protects the retained suffix, and successful free
clears the flag before reuse. Repeated shrink preserves the flag; later growth
copies only the logical prefix and frees the complete old extent.

Review found a related boundary gap in allocator transfer. When a side owner
refused free-tail publication, transfer discarded the source bump cursor
without retaining the last allocation's end. It now retains atomic boundary
sentinels on that refusal. This also protects earlier plain late frees whose
extent reconstruction uses the same boundary rule.

The new `m2_arena_plain_shrink` gate exercises old and newly admitted full-span
readers while shrink is paused, a marking lease, forced allocation failure,
root/recovery owners, an unrelated retained destructor, all six free entry
routes, repeated shrink followed by growth, the last active bump allocation,
a reused interior bump and refused tail publication during transfer. Reuse
checks require the entire original allocation to become available again.
Existing realloc, allocf, directory and reader-progress fixtures now check
retained storage instead of expecting a move or independently reusable suffix.

The implementation deliberately retains capacity while the smaller allocation
is live. Its maximum is the original small allocation, at most 16 KiB; this is
not a claim of stock memory parity. It avoids allocating or maintaining a new
per-allocation size record. Ordinary allocation gains no metadata RMW; ordinary
plain free gains a flag load, and flagged free scans only its bounded original
span. The measurements below keep both runtime cost and retained capacity
visible.

Five alternating process pairs on CPU 8 compare ordinary allocation/free at
64, 256 and 4,096 bytes. Median candidate/baseline paired ratios are 1.008,
1.001 and 1.001; the individual ratios span 0.986–1.008, 0.995–1.020 and
0.996–1.011 respectively. This narrow control finds little change in ordinary
free cost. It is not a stock comparison or a measurement of every allocator
workload.

Retained memory is a substantial cost for applications that keep shrunken
allocations alive. Three pairs per size/count allocate blocks, shrink them to
16 bytes, then allocate checked 1 KiB refills before freeing everything.

| Original allocation set | Physical bytes after shrink, baseline / candidate | Refilled RSS, baseline / candidate |
| --- | ---: | ---: |
| 8,192 × 4 KiB | 128 KiB / 32 MiB | 42.0 / 68.9 MiB |
| 2,048 × 16 KiB | 32 KiB / 32 MiB | 42.5 / 76.0 MiB |

All original allocation bits clear after full free. These large controls also
expose a preexisting bin-discovery problem in both versions: only about 8 MiB
remains linked in free bins after freeing the complete allocation set. The
8,192-node defensive bin-walk limit truncates valid long chains. Four separate
owner-only rebuild controls distinguish that loss of discoverability from
retained shrink extents. Candidate bin plus bump capacity recovers every usable
arena byte after rebuild, 59,132,160 and 65,479,680 bytes for the two large cases.
The baseline recovers most capacity, with residual gaps of 43,392 and 22,912
bytes. No automatic-recovery or long-run memory bound is claimed. Both bin
discovery and reducing retained live-shrink capacity remain performance work.
The [measurement report](evidence/plain-shrink-2026-11-26/performance/REPORT.txt)
preserves all 30 ordinary-cost processes, 36 retention processes, four rebuild
controls, the baseline witnesses, raw memory observations and exact identities.

Seven configurations pass: GCC release, Clang assertions, ASan, TSan, and
builds without FFI, JIT or buffers. The final verification counts 85 direct
runtime invocations and nine canonical gates. Release/assertion/ASan each pass
509 stock JIT source cases, 387 interpreter cases and 509 independently produced
stock-bytecode cases, plus the shared-runtime workloads. All twelve standalone
allocator fixtures pass under assertions, ASan and TSan; those configurations
also pass the new shrink, reader-progress and plain-late fixtures. Complete GC
traversal passes with assertions and ASan. ASan leak detection is disabled;
TSan does not instrument generated JIT machine code.

The first standalone assertion builds caught an include dependency introduced
by assertions in the new inline accessors. Removing those header assertions
preserves the lightweight standalone header; production C callers already
validate the small PLAIN start. The original failures, corrected build inputs
and passing reruns are retained. The
[verification record](evidence/plain-shrink-2026-11-26/review/final-verification.json)
checks 228 runtime inputs per configuration, executable/library hashes and the
restored default build. The
[manifest](evidence/plain-shrink-2026-11-26/manifest.json) separates copied text
from hashes of binaries and large source archives.

Huge allocation shrink is unchanged. It can still require replacement storage,
including during overflow-stack relimiting. The next design must separate
logical size from physical capacity, classify huge pointers safely after a
huge-to-small logical shrink, preserve full-span readers and unmap the complete
original mapping. Keeping a huge mapping indefinitely can retain far more than
16 KiB, so that tradeoff needs its own measurement. The
[worker-retention diagnostic](worker-retention-2026-11-26.md) records additional
independent reclamation gaps. These remain release blockers; the full runtime
lock-freedom and performance goals are not complete.

# Buffer retention and allocation costs

This follow-up starts at `51f8adc8e48e9d9904c769d42ce57ca5a408f9af` and keeps
the concurrent storage protocol from the
[buffer safety review](buffer-concurrent-storage-2026-11-26.md). It improves
Linux performance without introducing a private, unretained storage path.

Buffer admission now samples only the backing address, retains that exact
allocation, then reads the coherent backing/read/write tuple. It accepts the
tuple only when it names the retained allocation. No header access precedes
admission; all offsets come from the later tuple. Address reuse before admission
is harmless when the tuple names the newly retained incarnation, and the lease
excludes reuse afterwards. A null address observes an empty buffer. Copy-on-write
reads keep their additional owner lease and final tuple recheck. This removes
one locked 128-bit snapshot per admission.

Small-allocation leases that do not request a full span now inspect only the
cells needed to establish their minimum size. The allocation-start, mapping,
lifetime, type and final structural checks are unchanged. Callers requesting a
span still receive the same conservative extent. Even a one-byte request retains
the entire allocation until release.

When consumed-prefix exhaustion would replace an owned buffer without growing
its live content, the replacement reserves room for up to four incoming chunks.
Additional headroom stops at an 8 KiB payload, below the dedicated-mapping
threshold. Capacity remains rounded to powers of two. The policy does not add
headroom to ordinary growth, copy-on-write conversion or replacement solely due
to a busy writer. Published bytes are still never overwritten. Repeated 64-byte
refills settle at 256 bytes; 1 KiB refills settle at 4 KiB. This deliberately
trades bounded retained capacity for fewer replacements.

All seven configurations pass: GCC release, Clang assertions, ASan, TSan, and
builds without JIT, FFI or buffers. The release, assertion and ASan builds each
pass 509 stock source cases, 387 interpreter cases, 509 imported stock bytecode
cases and the shared-runtime CI workloads. Assertions and ASan also pass the
full GC traversal fixture. The canonical buffer publication, suspended-actor
and new small-lease gates pass and restore the default shared build.
[Final verification](evidence/buffer-lease-cost-2026-11-26/review/final-verification.json)
checks all 228 runtime inputs per build, executable/library hashes and 44 direct
runtime invocations; composite CI invocations contain further tests.

The buffer fixture now also pauses after sampling an address but before retaining
it. Peers advance offsets or free and replace storage before the writer resumes.
The new lease fixture uses real adjacent allocations at bitmap-word boundaries,
checks byte-sized minimums through the first rejected byte, and retains an entire
8 KiB body with a one-byte lease during free and allocation churn. Existing
suspended-writer, metadata-retirement, exact-consumption and large-buffer checks
remain enabled. ASan leak detection is disabled; TSan does not instrument JIT
machine code.

Paired GCC measurements on CPU 8 compare with the accepted preceding runtime:

| JIT workload | Previous ns/op | Current ns/op | Current / previous |
| --- | ---: | ---: | ---: |
| Reset/put/get, 64 bytes | 470.87 | 281.50 | 0.598 |
| Reset/put/get, 1 KiB | 1851.45 | 1598.15 | 0.863 |
| Append, 1 byte, quiet long run | 61.01 | 55.27 | 0.906 |
| Append, 16 bytes, quiet long run | 62.94 | 58.85 | 0.935 |
| FIFO get, 1 byte | 112.12 | 103.69 | 0.925 |
| Set/get, 64 bytes | 427.32 | 388.72 | 0.910 |

The [main report](evidence/buffer-lease-cost-2026-11-26/performance/combined/summary.txt)
retains all 152 initial rows, including substantial variation in long appends and
two interpreter reset outliers. A separate
[quiet append run](evidence/buffer-lease-cost-2026-11-26/performance/combined/quiet-append-summary.txt)
retains 20 more rows after compilation and other test activity ended. Its paired
ratios are 0.887–0.922 for one-byte appends and 0.896–0.942 for 16-byte appends.
The capacity-only experiment and all sequential stock/older-fork controls are
also preserved. The local stock reference has source and binary hashes but no
verified upstream commit; it is not identified as the design's pinned baseline.
Trace-exit witnesses cover append, reset/get, FIFO and set/get. Method decoding
remains interpreted in the fork but runs natively in the stock control, so that
comparison also includes a capability difference.

Performance completion remains open. The older fork controls still take about
3 ns per tiny append, and this change does not restore stock buffer throughput.
Allocation counters show that 10,000 warmed 1 KiB refill cycles drop from 10,000
allocations of 1,072 bytes to 2,500 allocations of 4,144 bytes. Allocated bytes
barely change: 10.72 MB versus 10.36 MB. Sustained 500,000-cycle profiles take
7.929 microseconds/cycle before and 6.888 afterwards, much more than the short
benchmark. In the candidate profile, `arena_find_run` accounts for 68.61% of
self cycles, followed by mark clearing and recovery cleanup. These are individual
profile samples, not paired throughput estimates. The
[cost investigation](evidence/buffer-lease-cost-2026-11-26/performance/lj-buffer-costs-20261126-kxgq98u7/REPORT.txt)
includes counters, trace dumps, helper assembly and profile output.
Allocation/reclamation under
sustained churn is the next performance and memory-stability investigation.

A sampled single-mutator flag was rejected as sufficient authority for a new
unretained fast path. First external attachment without existing traces does
not establish the required exclusion from a paused interpreter C operation.
The analogous existing table fast path needs a separate review. An isolated
diagnostic for that path was stopped by automatic safety review, which flagged
possible cybersecurity risk; its reproduction remains unconfirmed here.

The evidence manifest records 572 text artifacts and 61 hash-only artifacts.
Its SHA256 is `3e32731798194c61d01412e73b8e57380bd5a3732d9657656182fe4caf8bc449`.

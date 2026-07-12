# GC2 scoped mark-lease borrowing experiment (rejected, 2026-07-12)

An isolated experiment let synchronous child-mark helpers borrow an already
counted same-arena body admission instead of acquiring another admission. The
comparison used the exact current closure-performance stack in clean release
builds.

Eight alternating `closures_upval` pairs (each best of five, one million
operations per sample, `BENCH_SCALE=0.2`) measured a 480.43 ns/op baseline and
478.75 ns/op with borrowing. The paired mean improvement was 1.91 ns/op, or
0.40%. `perf stat -r3` agreed: task clock and core cycles fell about 0.36%, while
instructions increased 0.05%. A broader TLS-based version regressed about 3%.

The exact variant passed default and assert+paranoia recovery tests, and an
independent review found its current synchronous call graph sound. It still added
hot-path arguments and branches, crossed register limits in preservation calls,
grew `lj_gc2.o` text by about 280 bytes, and enforced non-escape only through the
current call graph.

Decision: do not integrate it. A reproducible 0.4% cycle reduction does not pay
for the extra protocol and maintenance surface, especially after the newer
pending-root cap, prototype completion stamp, and range-clear work absorbed most
of the original opportunity.

# Combined byte index/end and wider-forwarding ordinary checkpoint

Frozen source `ec98dbf5a` combines wide VARG/CALLM argument forwarding,
integer byte indices, and positive/numeric/negative dynamic byte ends with
the earlier NUM integer-loop-body and variadic aggregate-prefix work.
The integrated string-byte contract passes all four ARM64/ARM64e
assertion/release profiles, including the integer source tests and both
negative-end forms. This validates the resolved fixture merges as well as
the combined production code.

Ordinary assertion ARM64 and strict `-Werror` x86_64 builds each pass all 509
stock source and 509 original v2 bytecode cases, all 260 workload result
comparisons, and the topology result/error suite. The twenty sampled
workloads have native root-exit witnesses in eighteen rows on each target.
Matched probes retain fractional FORL, mixed CALLM producer, fixed byte-range
and NUM-loop/INT-body behavior. Integer and fractional byte indices of either
sign, and the positive, fractional, nil and negative dynamic ends, each have
eight selected root exits before and after full GC on both architectures.
The independently dynamic start/end row still lacks an ARM64 root and is
being addressed separately. These observations do not establish full parity.

Evidence: `/Users/frityet/Scratch/ljmt-byte-wide-combined-20261030/` contains
the frozen revision, ordinary matrix script, both architectures' build,
source/bytecode, workload/topology and focused probe logs, plus immutable
ordinary executables, archives and JIT modules with SHA-256 manifests.
The restored ordinary assertion ARM64 profile is
`f8de98b1a10b27c08499cc3c5e6df1e39d6e1d37ba47638f58d39d87ce97908d`.
The integrated four-profile byte log is
`/Users/frityet/Scratch/ljmt-wide-numeric-byte-integration-20261030/all-index-end-four-profiles.log`;
its restored profile is
`2c72e7758af664b4d9a39b8e4130a1668db186b52113f75f44ab35f3292b6aa7`.

The full gate runs separately on `ff86407a1`, with the `4b8c2f880` production
source and a corrected aggregate-prefix test expectation. It does not cover
the later additions in this checkpoint. Main remains on the completed
`89226814d` production checkpoint.

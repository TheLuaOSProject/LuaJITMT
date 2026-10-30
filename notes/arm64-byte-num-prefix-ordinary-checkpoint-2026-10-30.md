# Ordinary byte bounds, NUM integer-body and FFI prefix checkpoint

Frozen production source `4b8c2f880` combines the complete `89226814d`
checkpoint with guarded integer bodies inside NUM FORL, constant negative
string.byte indices (including folded -1 and same/nil ends), dynamic positive
integer byte-range ends, and variadic indirect aggregate results with named
aggregate prefixes, including a prefix after eight occupied GPRs.

The ordinary assertion ARM64 and x86_64 builds both pass all 509 stock source
tests and 509 stock v2-bytecode tests. The x86_64 build uses `-Werror`. Both
pass the twenty-row workload inventory and expanded topology result/error
comparisons. This is ordinary-build evidence; it is not a full parity claim.

Matched probes confirm NUM-index/INT-accumulator native roots and the same
overflow result `2147483650`. The existing observer count remains eight normal
and seven post-GC ARM64 exits versus eight/eight on x86_64, also seen in the
earlier integer-control probe. All six mixed CALLM producer rows retain eight
selected exits before and after GC. Positive fixed/nil range ends, the
constant-negative sample and positive dynamic integer ends each have eight
selected exits before and after GC on both targets. Numeric and negative
dynamic end recordings, and independently dynamic start/end, remain separate
ARM64 gaps.

Evidence directory:
`/Users/frityet/Scratch/ljmt-byte-num-prefix-integration-20261030/`.
It contains `{arm64,x86_64}-{build,source,bytecode}.log`, parity/topology TSVs,
the focused probe logs, and preserved `*-ordinary/` executables, archives,
JIT modules, revision records and SHA-256 manifests. Initial build profiles:

- ARM64: `fbd542645090148ef57c8dae53f4f4c72d91aeeb50865089a3209e21654796ff`.
- x86_64: `441dbdc81a3988cb325fcd90aca6ed12b28d1de962da9987651f7701c549d4e5`.

After all tests completed, the scratch matrix script mistakenly unset a
profile variable needed by its final restoration. That failed only the
restoration step. The script now preserves the original variable; the stale
lock owned by the exited process was retained in the evidence directory, and
an independent ordinary ARM64 rebuild succeeded. See
`ordinary-arm64-restoration-retry.log`, profile
`af101409d8bb9ea7ba3650716e01be6beb48704afe3d3a860c67d8897cbcb9c9`.

The full combined gate is running separately at `9581ed718`, whose production,
tests and CI scripts are identical to `4b8c2f880`. The main parity checkout
remains on the already complete `89226814d` production checkpoint.

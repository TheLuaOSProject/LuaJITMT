# Wider forwarding, byte ranges and FFI ordinary checkpoint

Frozen production and tests at `d0ee5eb8c` pass an ordinary assertion ARM64
build and a strict `-Werror` x86_64 build. Each passes all 509 stock source
cases and all 509 original v2-bytecode cases. The workload probe has 260
semantic checks and 18 of 20 sampled rows with a root-exit witness on each
architecture. Topology, fractional FORL, CALLM, fixed/dynamic byte bounds and
integer-accumulator probes also complete with interpreter-oracle checks.

The combined source includes wide fixed vararg forwarding, positive/negative
INT byte indices, numeric/negative byte ends, positive dynamic start/end
ranges, captured FFI prefixes, folded 64-bit literal arguments, and literal
aggregate-prefix composition coverage. Their focused four-profile evidence
is recorded in the component notes. This ordinary run does not replace the
full gate, which is running separately on the same frozen revision.

Evidence and immutable binaries, archives and matching JIT modules are in
`/Users/frityet/Scratch/ljmt-wide-byte-ffi-complete-20261030/`.
`source.txt` identifies the ordinary revision; `ordinary-matrix.sh` records
the commands. `{arm64,x86_64}-{build,source,bytecode}.log`, parity/topology
TSVs and focused probe logs retain the results. The runner reports
`ordinary matrix passed at d0ee5eb8c` and the ordinary ARM64 restoration
passes. Newer notes do not change the tested source.

An additional live-peer, observer-free topology survey is in
`{arm64,x86_64}-expanded-parity.tsv`, with source at
`/Users/frityet/Scratch/ljmt-expanded-parity-20261030/probe.lua`.
It identifies native x86_64 roots missing on ARM64 for fixed multiple-byte
returns, dynamic-end multiple-byte returns, pure leaves with empty branch
arms or sequential conditions, string.sub, math.modf, and the string-valued
tostring path. Root bytecode and observed exits are recorded separately;
math.modf's published x86_64 root has no exit witness in this survey. Several
other candidates (protected leaf loop, explicit next and a new-table loop)
also lack an x86_64 witness and are not established native parity gaps by
this probe. These observations are scoped to these source shapes and builds.
Full x86_64 parity is not complete.

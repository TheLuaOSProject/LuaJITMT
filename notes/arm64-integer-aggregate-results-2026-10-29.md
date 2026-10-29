# ARM64 one-GPR integer aggregate FFI results

This slice records direct Darwin ARM64 integer structs returned in one GPR when
their size is exactly 1, 2, 4, or 8 bytes. Each admitted return has a separately
classified integer/boolean/enum field layout, no union, bitfield, nested or
floating members, no reference fields, and no alignment above eight bytes.
The call returns raw U8/U16/U32/U64 IR only long enough to store the exact
width into a newly allocated CData payload. The Lua-visible value remains the
declared aggregate CData rooted through XSAVE and the native-frame lifecycle.

The recorder preserves the declared result CType for CNEW. The ABI signature
also carries the resolved result type and size. The semantic pass independently
reconstructs the CALLXS signature, CNEW, exact payload address and XSTORE, and
requires every result snapshot to retain the CData box without exposing the raw
CALLXS carrier. The post-RA pass separately checks the same source binding,
snapshot, type, register/spill, and payload-store conditions. The existing
native enter/leave pin and lockless root protocol is unchanged.

The native fixture covers all four widths with a live MT peer, oracle equality,
forced selected-root exits, exact native effects, and native re-entry after full
GC. It also covers a typedef return and rejects an over-aligned return. Private
semantic and post-RA mutations independently reject a raw-carrier snapshot,
wrong XSTORE width, a different valid CNEW CType constant, and an XSTORE bound
to another IR value.

This does not admit HFA or other floating aggregate results, multi-GPR results,
indirect/sret results, stack returns, or arbitrary aggregate layouts. Four
The focused integer-aggregate contract passed on ARM64 and arm64e assertion and
release profiles, including the pre-existing integer-aggregate argument and
HFA argument cases. Ordinary binaries built with assertions also passed the
retained-result probe on both architectures against the same universal library.
The ARM64 retained integer and word results each published a selected root with
two CALLXS, two CNEW and two XSTORE nodes, then took the recorded root exit;
x86_64 also retained integer, SSE and word result roots. The ordinary probe
checked 25 candidate/oracle comparisons and 2752 foreign calls per row. Exact
runtime, archive, profile, library, and probe hashes plus output are preserved
under `/Users/frityet/Scratch/ljmt-int-aggregate-results-20261029`.

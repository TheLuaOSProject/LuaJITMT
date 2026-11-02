# Pointer-bearing ARM64 indirect aggregate results

The bounded Darwin ARM64 indirect aggregate result classifier now accepts
pointer leaves in 17–32-byte non-union structs. The result walker already
validated pointer scalars for direct one-GPR aggregate results; this change
enables that same scalar leaf kind for the X8 indirect-result path. The
existing constraints still require a bounded CType walk, native size matching,
alignment no greater than eight bytes, and at least one non-floating leaf so
an HFA cannot be mistaken for a non-HFA sret result. Pointer fields remain
opaque bytes in the result CData. The current walk follows scalar and nested
struct fields only; arrays, unions, references, over-alignment, vectors,
complex members, and result layouts above 32 bytes remain closed. Pointer
arguments and general aggregate/sret combinations are unchanged.

The native fixture returns a 24-byte `{ uint64_t, void *, uint64_t }` from a
compiled C helper. The traced CALLXS loop compares all fields with a JIT-off
interpreter oracle, checks a changed result signature exits, then forces full
GC and requires the same selected native root to re-enter without changing its
machine-code identity. Exact C call counts detect lost or replayed effects.
The copied-trace test independently admits the semantic and post-RA forms,
then applies the existing result-box CType, hidden payload, CALLXS binding,
snapshot, and physical-location mutations to this two-argument signature.

`JOBS=2 tools/ci/arm64_jit_integer_aggregate_args_contract.sh` passed all four
ARM64/ARM64e assertion/release profiles on 2026-11-02. This is a bounded
extension of the existing sret class; unions, over-aligned values, vector or
complex members, array fields, result layouts above 32 bytes, and general
aggregate/sret combinations remain closed.

The durable four-profile log is
`/Users/frityet/Scratch/ljmt-pointer-sret-20261102/four-profile.log`. The
helper-free comparison log and signed binaries are under
`/Users/frityet/Scratch/ljmt-pointer-sret-20261102/ordinary/`; both fresh
ordinary ARM64 and freshly rebuilt/ad-hoc-signed x86_64 used the same universal
C helper (SHA-256 `a2eb4b0e09040a133ec8276b9a1e96cbf215de532f464c20594424537fbba500`).
The `pointer-sret` row on each architecture published selected trace 1,
observed one selected-root exit after full GC and 31 alternate-callee exits,
and matched all result fields against the interpreter oracle. The test checked
1,024 warmup effects, 64 post-GC effects and 64 alternate-callee effects,
including no replay or loss. Runtime and archive hashes are in `sha256.txt`;
`x86_64-codesign.txt` records the ad-hoc signature.

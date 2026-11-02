# Pointer-bearing ARM64 indirect aggregate results

The bounded Darwin ARM64 indirect aggregate result classifier now accepts
pointer leaves in 17–32-byte non-union structs. The result walker already
validated pointer scalars for direct one-GPR aggregate results; this change
enables that same scalar leaf kind for the X8 indirect-result path. The
existing constraints still require a bounded CType walk, native size matching,
alignment no greater than eight bytes, and at least one non-floating leaf so
an HFA cannot be mistaken for a non-HFA sret result. Pointer fields remain
opaque bytes in the result CData. Argument classification is unchanged.

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
complex members, result layouts above 32 bytes, and general aggregate/sret
combinations remain closed.

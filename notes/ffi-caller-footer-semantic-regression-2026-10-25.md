# Semantic CALLXS caller-footer regression (2026-10-25)

`tests/t-arm64-jit-ffi-abi-classes.c` now checks caller identity in the semantic
IR walk as well as in post-RA admission. The semantic test reconstructs a
private trace view from the published trace, restores the source `FORL` word
only while invoking the pre-publication semantic checker, and restores the
live `JFORL` word before any later checker or Lua execution. Each mutation
starts from a newly reconstructed view that first passes semantic admission.

Two malformed-but-plausible cases must be refused: changing only the caller
snapshot footer to another valid `BC_CALL` site, and changing that footer plus
the matching KNUM payload to the same alternate site. This verifies that a
paired payload cannot make a footer from another callsite authoritative.

Constant-pool walks cover `[nk, REF_TRUE)`, excluding canonical primitive
references and IR_BASE. Wide KNUM/KINT64 and GC64 pointer-like constants
consume two slots; their next reference may equal `REF_TRUE`, but their payload
must precede it. The integration review corrected an unnecessarily extended
`REF_FIRST` boundary and preserved the actual signed frame-depth field widths
when saving recorder state.

Validation: the complete focused `m7_ffi_callxs_arm64_scalar` contract passed
on native ARM64 with the semantic mutations enabled. The arm64e variant remains
pending; its shared-library target propagation is being fixed centrally.

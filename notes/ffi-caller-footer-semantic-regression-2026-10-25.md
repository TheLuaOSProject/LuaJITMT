# Semantic CALLXS caller-footer regression (2026-10-25)

`tests/t-arm64-jit-ffi-abi-classes.c` now checks caller identity in the semantic
IR walk as well as in post-RA admission. The semantic test reconstructs a
private trace view from the published trace, restores the source `FORL` word
only while invoking the pre-publication semantic checker, and restores the
live `JFORL` word before any later checker or Lua execution. Each mutation
starts from a newly reconstructed view that first passes semantic admission.

Two malformed-but-plausible cases must be refused: changing only the meta
snapshot footer to another valid `BC_CALL` site, and changing that footer plus
the matching KNUM payload to the same alternate site. This verifies that a
paired payload cannot make a footer from another callsite authoritative.

The fixture also bounds constant-pool walks by `REF_FIRST`, the end of the
constant region, rather than `REF_TRUE`, the first true-valued IR reference.
Wide KNUM/KINT64 (and GC64 pointer-like) constants consume two IR slots; the
walk checks that the second slot remains within the constant pool before
reading it. This avoids interpreting payload words as independent constants
or reading beyond the pool while preparing the semantic mutation view.

Validation: the complete focused `m7_ffi_callxs_arm64_scalar` contract passed
on native ARM64 with the semantic mutations enabled. The arm64e variant remains
pending; its shared-library target propagation is being fixed centrally.

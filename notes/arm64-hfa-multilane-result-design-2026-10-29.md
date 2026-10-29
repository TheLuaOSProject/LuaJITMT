# ARM64 multi-lane HFA result design

The existing CALLXS/HIOP lowering is a pair contract, not an extensible list.
`asm_setupresult()` in `src/lj_asm_arm64.h` looks only at the instruction
immediately following CALLXS and assigns that HIOP to D1. The `IR_HIOP`
assembler path in `src/lj_asm.c` expects its immediate predecessor to be the
low CALLXS result. The backward allocation hints and `asm_hiop()` use that
same adjacency; DCE treats HIOP as a store-like operation and has special
pair liveness. Chaining HIOPs or raising the HFA count would violate those
assumptions.

A generic three/four-member result therefore needs an explicit backend-owned
result group. The group must bind to one CALLXS and describe ordered typed
lane definitions, offsets and carrier locations. A suitable lowering must
reserve D0-D3 before argument/scratch allocation, keep the CALLXS live when
any lane is live, and preserve every lane across native leave until its exact
XSTORE. Its optimizer/DCE rules must keep or remove the group atomically, and
snapshots must reject every raw lane. The independent semantic walk should
derive group geometry from the guarded CType and CALLXS; the post-RA walk
should independently verify complete ordered lanes in D0-D3, exact stores,
and no raw snapshot roots.

Do not encode three/four lanes as consecutive HIOPs without first replacing
the adjacency-based assembler and allocation rules. The two-double result
path stays within the existing, proven two-register pair contract.

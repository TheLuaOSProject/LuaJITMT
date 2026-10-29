# ARM64 pointer-field aggregate results

ARM64 small direct aggregate results may now contain pointer leaves. The
pointer bits remain an opaque `IRT_U64` result carrier stored in the existing
result CData box. In `LJArm64SmallResultWalk`, pointer leaves are opt-in and
enabled only for the one-GPR result classifier (sizes 1, 2, 4, or 8); the
17–32-byte indirect-result classifier leaves that option disabled. The
argument classifier is unchanged.

The native fixture returns `struct { void *value; }`, with both NULL and
non-NULL values, and an alternate callee of the same result type. It compares
each result to an independently JIT-disabled function, holds an MT peer,
checks actual C effect counts, requires a selected `CALLXS` loop root and
selected-root exits after full GC and after the alternate callee, and verifies
the root's machine-code pointer, address and loop remain unchanged. The test
also exercises the scalar alternate-result exit.

Independent semantic and post-allocation clone tests reject changing the
result type from U64 to PTR, changing the box CType, corrupting its snapshot
root, changing the store width/source, and placing the result in an FPR.
Classifier checks accept the direct pointer-field result, while pointer-field
by-value arguments and pointer-containing 24-byte sret results remain refused.

## Validation

- `JOBS=2 tools/ci/arm64_jit_integer_aggregate_args_contract.sh` passed all
  four ARM64/ARM64e assertion/release profiles. Final log:
  `/Users/frityet/Scratch/sret-profile-tmp/pointer-result-final-four-profile.log`.
- Matched ordinary no-helper Lua probes passed on ARM64 and x86_64. Both
  reported selected root 1, one selected exit after GC, 31 alternate-callee
  exits, and stable code identity. The live peer joined on each run. Logs:
  `/Users/frityet/Scratch/sret-profile-tmp/pointer-result-ordinary-arm64.log`
  and `pointer-result-ordinary-x86_64.log`.
- The ordinary ARM64 runtime was built from this source with assertions and
  no test helper macros; SHA-256:
  `e01f14ef04180b8d49884876babe6195cc3c69cdba06f263a7bbef36481cc922`.
  The frozen ordinary x86_64 runtime was from the matched Dae bundle at
  `dae24b46aad9d4fc1d36cdce5b94d49d05d4743a`; SHA-256:
  `5998c9d47d84d717619d0ef50e339550c9cbe361fd2cbf3fcf8a6fadc4cfe04c`.
- The universal helper library SHA-256 is
  `b93cb3d9f2caa1847a26373034235704d2597910ae18973601ee984ef1b589fd`.
  The probe library and both runtime copies are preserved in
  `/Users/frityet/Scratch/sret-profile-tmp/`; the x86_64 runtime's matching
  module bundle is under `/Users/frityet/Scratch/ljmt-side-ancestry-20261029/dae-x64/`.

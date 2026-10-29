# Literal narrow CALLXS stack regression (2026-10-29)

Added a focused regression for fixed 1- and 2-byte scalar arguments after the
Darwin ARM64 GPR argument bank is full. It includes a hardcoded `uint8_t`
prefix before an 8-byte integer aggregate and alternating signed/unsigned
byte and halfword arguments around that aggregate. The interpreter oracle
checks the actual C values and return result; the contract also requires a
selected candidate root with CALLXS/XLOAD and native-finish evidence, forced
exit/re-entry after full GC, live-peer coverage, and independent semantic and
post-RA rejection when distinct source argument pairs are swapped.

No production change was needed at this checkpoint. Recorder IR carries these
fixed stack arguments through the existing typed U8/U16 conversions, and the
existing ABI-width emitter handles their packed stack placement. A direct
KINT recorder experiment was rejected and reverted: it removed the native
witness. The fixture therefore does not claim direct-KINT carrier support.

Validation: `tools/ci/arm64_jit_integer_aggregate_args_contract.sh` passed all
four ARM64/ARM64e assertion and release profiles. The focused ARM64-assert
profile also passed independently. Ordinary no-test-helper ARM64 evidence was
collected with a clean build at the branch base `9d17d2e7f`; the live-peer
candidate using the hardcoded literal prefix published root 1 (`nins=75`),
whose IR contained two CALLXS instructions. After full GC, the same trace
remained available and reported eight selected-root `texit` events with the
peer still attached. The probe, ordinary output, and test library are in
`/Users/frityet/Scratch/ljmt-constant-stack-debug/`. The older baseline refusal in
`notes/arm64-small-stack-composites-2026-10-29.md` predates that candidate
probe and should not be read as evidence that the literal form remains
unsupported at this branch base.

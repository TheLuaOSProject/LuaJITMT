# Positive dynamic range starts for string.byte

ARM64 previously admitted a constant start or a dynamic two-argument index,
but refused positive ranges whose start was dynamic. This slice accepts the
recorded `string.byte(s, last, last)` and `string.byte(s, first, last)` forms
when both indices are exact guarded source parameters. The start and end may
be integer SLOADs or the recorder's checked NUM-to-INT conversion of guarded
number parameters.

The semantic and post-allocation validators independently bind the source
CALL's three argument slots, the same string's guarded length, the unsigned
`end <= length` guard, the checked `first - 1` normalization and nonnegative
guard, and the checked `end - offset` width with a positive equality guard.
Together these establish `0 <= first-1 < end <= length` before STRREF. The
existing source, snapshot, allocation, and byte-load checks remain in force.
No emitted instructions or runtime synchronization were added.

The follow-up binds each conversion check to the exact first or end reference
in this range and each unsigned-guard check to the exact `end <= length`
guard. This prevents an unrelated conversion or guard from borrowing a valid
range certificate. It also requires the normalization and nonnegative guards
to precede the checked width operation. The source CALL proof scans all
bytecode jumps and refuses a branch into the middle of the three-instruction
argument setup while allowing entry at the setup's first instruction.

## Validation

- `tools/ci/arm64_jit_string_byte_contract.sh` passed assertion and release
  builds for ARM64 and ARM64e/BTI. The C fixture covers same-start/end and
  independent-start/end forms with both integer and NUM inputs, compares
  errors and values against separately interpreted functions, exercises
  native exits and full-GC code reuse, and independently mutates range guards,
  width, string length binding, and source index slots in both admission
  passes.
- The ordinary baseline probe before this change showed both forms selecting
  a native x86_64 root while ARM64 did not. Its logs are
  `/Users/frityet/Scratch/ljmt-byte-num-prefix-integration-20261030/arm64-dynamic-end-string-byte.log`
  and
  `/Users/frityet/Scratch/ljmt-byte-num-prefix-integration-20261030/x86_64-dynamic-end-string-byte.log`.
- After the change, the ordinary live-peer probe selected root 1 on both
  ARM64 and the frozen x86_64 binary for integer and NUM same-start/end and
  independent-start/end forms. It varied start and end separately, checked
  interpreted values and errors for empty/short strings and invalid ranges,
  verified one versus three returned byte values, and observed selected-root
  exits before and after full GC. Exit totals differ by target and are not
  treated as a trace-topology equivalence claim.
- Both ordinary targets passed all 509 stock source tests and all 509 genuine
  v2 bytecode tests. Logs are
  `/Users/frityet/Scratch/ljmt-byte-start-end-final-arm64-source-20261030.log`,
  `...-arm64-bytecode-20261030.log`,
  `...-x64-source-20261030.log`, and `...-x64-bytecode-20261030.log`.
- The final four-profile contract log is
  `/Users/frityet/Scratch/ljmt-byte-start-end-bound-followup-contract6-20261030.log`.
  An earlier fixture-only failure was caused by omitting the original proto
  from the allocated view; the fixture now supplies it, matching the other
  post-RA admission fixtures.
- After certificate-binding hardening, the ordinary selected-root probe was
  repeated on ARM64 and frozen x86_64. Logs are
  `/Users/frityet/Scratch/ljmt-byte-start-end-bound-followup-arm64-ordinary-20261030.log`
  and
  `/Users/frityet/Scratch/ljmt-byte-start-end-bound-followup-x86_64-ordinary-20261030.log`.

This is a bounded positive-range proof, not general dynamic range parity. The
full combined gate has not been run on this isolated follow-up.

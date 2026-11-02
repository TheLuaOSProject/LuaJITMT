# ARM64 FFI large indirect aggregate results

The Darwin ARM64 `CALLXS` sret class no longer has an arbitrary 32-byte
extent cap. `LJArm64ABISignature.result_size` now retains the full `CTSize`
instead of narrowing it through `uint8_t`; the admission predicate accepts
the full valid extent after independently reclassifying the signature.
Recording still allocates the exact declared CType with ordinary fixed CNEW
and passes its rooted payload as the hidden X8 result pointer. This class
rejects over-alignment, so it does not use the explicit-size CNEW/newv or
signed-`IR_KINT` path.

Classification remains deliberately bounded: ordinary named structs only,
alignment at most eight, larger than 16 bytes, no more than 32 scalar leaves
or 32 nesting levels, valid non-overlapping in-bounds field extents, and at
least one non-floating leaf. Nested structs and opaque pointer leaves are
walked. The HFA classifier runs first; pure HFA results retain their existing
classification. Fixed-size arrays and nested arrays recurse over their
declared element types after checking nonzero element size, exact divisibility,
matching natural alignment and remaining leaf budget. Count is derived by
division and bounded before traversal, so a forged huge count cannot cause
unbounded work. Arrays of pointers are admitted only as opaque result leaves.
Unions, references, VLA/vector/complex members, unsupported leaves, and
over-aligned layouts remain excluded. Existing fixed CNEW allocation checks
continue to enforce the runtime allocation limit. The final field end must
round to the declared struct size at its natural alignment, using checked
arithmetic; this rejects oversized trailing extents and prevents
offset-plus-field-size wrap.

The ARM64/x86_64 companion checks nested 48-byte and 256-byte returns against
an interpreted oracle. On ARM64 each selected trace remains
published, exits and re-enters after a full GC, and records exactly the
expected native effects. The 48-byte case checks all six fields; the 256-byte
type compares all 32 nested `uint64_t` fields and exercises the former
`uint8_t` truncation boundary. Distinct same-type alternate callees prove the
signature guard exits. A valid 24-byte struct with four bytes of normal tail
padding checks the size-alignment rule. Fixed-array cases cover 40-byte integer
arrays, a 48-byte nested array-containing result, and a 40-byte pointer array.
Each compares every field, survives selected-root GC exits, and exits on a
distinct same-type alternate callee. The existing 8-byte and 24-byte
integer-array signature fixtures exercise direct GPR and indirect X8 classes;
the 8-byte array also runs through the native helper with both callees, checks
both returned words against the interpreted oracle, and verifies call effects.
The classifier fixture mutates and restores the 256-byte CType's
declared extent and final field offset near `CTSIZE_INVALID`, then mutates and
restores an array extent to a non-divisible size and a huge divisible count;
all malformed signatures are rejected before traversal can wrap or run
unbounded.

Validation logs:

- Four ARM64/arm64e assertion/release profiles: `/Users/frityet/Scratch/ljmt-large-sret-20261102/arm64-four-profile.log`.
- Exact `CTSIZE_INVALID` malformed-extent rerun in the ARM64 assertion profile:
  `/Users/frityet/Scratch/ljmt-large-sret-20261102/arm64-malformed-extent.log`.
- Four ARM64/arm64e profiles with fixed-array sret cases:
  `/Users/frityet/Scratch/ljmt-large-sret-20261102/arm64-array-four-profile.log`.
- Final four-profile rerun including the direct 8-byte integer-array native
  witness: `/Users/frityet/Scratch/ljmt-large-sret-20261102/arm64-array-final-four-profile.log`.
- Fresh x86_64 assertion runtime, ad-hoc signed and run under Rosetta with the
  same companion test: `/Users/frityet/Scratch/ljmt-large-sret-20261102/x64-comparison.log`.
- Final signed x86_64 comparison including direct and indirect array results:
  `/Users/frityet/Scratch/ljmt-large-sret-20261102/x64-array-final-comparison.log`.
- x86_64 signature details and runtime/archive/helper hashes are in the same
  directory (`x64-codesign.txt`, `sha256.txt`). The x64 run passed nested,
  pointer sret, 48-byte, 256-byte and padded-extent cases with selected trace
  1; the large cases observed one post-GC selected-root exit and 31 exits on
  the distinct same-type alternate callee.

The implementation covers only this bounded non-HFA result slice. It does not
open by-value aggregate arguments or general aggregate/sret combinations.

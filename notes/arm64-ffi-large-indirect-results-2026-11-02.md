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
classification. Arrays (including arrays nested in structs), unions,
references, VLA/vector/complex members, unsupported leaves, and over-aligned
layouts remain excluded. Existing fixed CNEW allocation checks continue to
enforce the runtime allocation limit.

The helper-free ARM64/x86_64 companion checks nested 48-byte and 256-byte
returns against an interpreted oracle. On ARM64 each selected trace remains
published, exits and re-enters after a full GC, and records exactly the
expected native effects. The 256-byte type contains 32 nested `uint64_t`
leaves, exercising the former `uint8_t` truncation boundary. The existing
classifier negative for a 24-byte array result confirms arrays remain closed.

Validation logs:

- Four ARM64/arm64e assertion/release profiles: `/Users/frityet/Scratch/ljmt-large-sret-20261102/arm64-four-profile.log`.
- Fresh x86_64 assertion runtime, ad-hoc signed and run under Rosetta with the
  same helper-free test: `/Users/frityet/Scratch/ljmt-large-sret-20261102/x64-comparison.log`.
- x86_64 signature details and runtime/archive/helper hashes are in the same
  directory (`x64-codesign.txt`, `sha256.txt`). The x64 run passed nested,
  pointer sret, 48-byte and 256-byte cases with selected trace 1; the large
  cases each observed the selected-root exit after GC and alternate-callee
  guard exit.

The implementation covers only this bounded non-HFA result slice. It does not
open by-value aggregate arguments or general aggregate/sret combinations.

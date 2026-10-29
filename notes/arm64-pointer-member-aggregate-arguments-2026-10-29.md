# ARM64 pointer-member aggregate arguments

The ARM64 FFI path now admits one additional fixed by-value argument shape:
an ordinary naturally aligned eight-byte struct with exactly one `void *`
member at offset zero. The planner treats its bytes as an opaque U64 GPR
payload. When X0–X7 are already occupied, the same payload occupies one
eight-byte stack slot; the following scalar starts at the next slot boundary.

This is an argument-only extension. It does not change pointer-containing
aggregate result classification, indirect-result handling, unions, nested
pointer structs, over-aligned forms, or pointer-bearing varargs. The source
CData remains in its original Lua frame slot as the GC root; the scalar
XLOAD carrier is used only for the native call.

The independent semantic and post-RA admission paths reconstruct the same
field layout and CType identity. The native fixture checks NULL and non-NULL
payloads, an alternate callee exit, full-GC reuse, live-peer execution, and
same-size CType substitution rejection. It also fills all eight GPR argument
slots before the pointer aggregate, checks the aggregate at stack offset 0
and the trailing scalar at offset 8, and exercises both pointer values through
native calls and GC reuse. The copied-trace admission checks retain the
existing independent source, payload width, source-slot, stack-plan, and
physical-location mutations.

Matched ordinary x64 and ARM64 probes used the same two-argument DSO/workload
and an independently compiled stack-boundary DSO/workload. Both architectures
published the selected root, recorded selected-root exits, and matched the C
oracle. Outputs were: two-argument ARM64 `root=1`, 24 selected exits, 64 C
effects; two-argument x64 `root=1`, 1 selected exit, 64 effects; stack-boundary
ARM64 and x64 each `root=1`, 5 selected exits, and 416 effects. The baseline
before this change had no ARM64 root for the two-argument case while x64 did.
Preserved ordinary evidence is under
`/Users/frityet/Scratch/ljmt-pointer-aggregate-args-20261029/`; x64 used the
frozen Dae runtime at
`/Users/frityet/Scratch/ljmt-side-ancestry-20261029/dae-x64`.

The full `arm64_jit_integer_aggregate_args_contract.sh` run passed all four
profiles after the natural-alignment restriction: arm64 assert `9d36e6ef`,
arm64e assert `4fc3cc4c`, arm64 release `3d929d2c`, and arm64e release
`ef2cd3b2`. The script restored the checkout to its arm64 assert profile
afterward. Ordinary ARM64 probes were rerun against that restored runtime
(SHA-256 `45ea3ff4bddf21248aa388fcae1268a64aba9b39fcb67dc3ce6eb21cbb191dd7`).
`git diff --check` was clean.

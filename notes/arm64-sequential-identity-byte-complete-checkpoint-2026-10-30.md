# Sequential leaves, numeric identities and byte-pair complete checkpoint

Production source `9ebec6139` passes the complete ARM64 experimental gate.
It extends the preceding verified `52efb731e` checkpoint with two/three/four
sequential pure numeric leaf decisions, guarded numeric identity calls,
zero-first substring normalization and exact fixed-pair raw byte snapshots.
The fixed-pair proof binds callee source copies, function identity, result
lanes and control-flow paths independently before and after allocation.
Existing allocators, emitters and the lockless architecture are preserved.

## Complete gate

The frozen checkout ran `tools/ci/arm64_jit_fail_closed_gate.sh` successfully,
including all four M7 FFI assertion/release ARM64/arm64e profiles, the complete
Lua-call and string contracts, and actual VM/recorder safepoint, upvalue,
cell-loop and native-coverage runtimes. The enclosing ordinary ARM64 restore
also returned zero. Source, gate and restore logs are under
`/Users/frityet/Scratch/ljmt-byte-pair-identity-complete-20261030/`.
The ordinary assertion ARM64 restore profile is
`5b19362d055752018f91036a4c9556078fe4e702e7f164b75000874d1b38469e`.

## Ordinary comparison

The matching exact-source ARM64 and strict x86_64 ordinary matrix passes
both 509-case stock corpora and all previous inventory, topology, byte, FFI,
substring, sequential-leaf and numeric-identity probes. The 24 numeric identity
rows and 16 zero-first substring rows retain their individual native witnesses.
The fixed-pair probe checks two raw loads, selected-root exits, independent
branch results, short and wrong-type inputs, NUL/high bytes, and unchanged
native code after peer GC. Both architectures observed 5,249 selected exits
in that probe. Binaries, modules, archives, hashes and logs are saved under
`/Users/frityet/Scratch/ljmt-byte-pair-identity-ordinary-20261030/`.
The ordinary matrix restored ARM64 profile
`7358f693d5166ef42b8040544b5255bf1074b9027ffbf0542ac3a574c72341c0`.

This gate excludes later negative-first substring allocation, computed math
result preservation, numeric-to-string conversion and child-frame side work.
The port remains experimental and full x86_64 parity is not yet established.

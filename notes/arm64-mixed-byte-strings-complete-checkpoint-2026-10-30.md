# Complete mixed byte and string checkpoint

The complete `tools/ci/arm64_jit_fail_closed_gate.sh` passes at frozen source
`503c8ba2f72619ef35ec7c5b590c600de7baa5b5`. This extends `8123b7a3d` with
mixed-sign multiple byte results, bounded positive-literal `string.sub`
allocation, and string-input `tostring` identity with exact source/result
binding. The substring path uses the existing string allocator and ARM64
emitter; no runtime locks are added. Fresh publication and existing-string
match hooks are test-only and absent from the ordinary build.

All four ARM64/arm64e assertion/release FFI profiles pass, followed by actual
VM/recorder safepoint runtime execution, upvalue identity, source/import loop
cells and native-coverage checks. The outer runner returned
`gate exit=0; restore exit=0`. Ordinary assertion ARM64 was restored without
test helpers, profile
`1df257f5f87f6a7b907d31c1503a6963bc78a238dfd9f0b844aa734cb8f0672c`.
Evidence is in
`/Users/frityet/Scratch/ljmt-mixed-byte-substring-complete-20261030/retry-503c8ba2f/`:
`gate-source.txt`, `complete-gate.log`, and `gate-restoration.log`.

The first gate at `4b494fcc1` correctly refused a malformed synthetic SNEW but
failed its obsolete expected-reason assertion (OPCODE rather than OPERAND).
The fixture expectation was updated at `768d30b00`; admission was not loosened.
That original failed log remains in the parent evidence directory.

The ordinary matrix at this exact source passes on ARM64 and strict x86_64:
509 source cases, 509 original v2-bytecode cases, 260 semantic workload checks,
topology and expanded probes, ninety fixed/dynamic-positive byte rows, forty
negative byte rows, eighty mixed-sign typed byte rows, the single raw-U8
snapshot witness, twelve FFI-index rows, sixteen positive substring rows and
both tostring loop forms. The inventory witnesses native root exits in
eighteen of twenty rows. The U8 probe observes 84 selected-root exits and
stable machine code after GC on both architectures. The string probes use
live peers, interpreter oracles and selected native roots with stable code.
Saved binaries, matching JIT modules, archives, hashes and logs are in
`/Users/frityet/Scratch/ljmt-mixed-byte-substring-tostring-ordinary-20261030/`.

Promotion covers this frozen source only. Negative and omitted-end substring
ranges, fixed-pair raw U8 snapshots and sequential leaf branches are later
work. General Lua calls/returns, recursion, protected calls, remaining FFI
forms and broader trace topology still prevent full x86_64 parity.

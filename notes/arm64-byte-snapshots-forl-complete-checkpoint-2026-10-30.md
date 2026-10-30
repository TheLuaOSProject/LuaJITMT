# Complete byte snapshots and FORL widening checkpoint

The complete `tools/ci/arm64_jit_fail_closed_gate.sh` passes at frozen source
`8123b7a3dd0d9c3d762675c6a5180e0230ddc21e`. This extends `789c1e229` with
dynamic positive-first and negative-first multiple byte results, raw U8
snapshots for the bounded single-result byte shape, and source-proven
nonnegative FORL index widening for I64/U64 FFI arguments. The changes keep
the lockless runtime and add no emitter or snapshot-restoration changes.

All four ARM64/ARM64e assertion/release FFI profiles pass, followed by actual
VM/recorder safepoint runtime execution, upvalue identity, source/import loop
cells and native-coverage checks. The outer runner returned
`gate exit=0; restore exit=0`. Ordinary assertion ARM64 was restored without
test helpers, profile
`f494b9ee6ebf7e0f479566b7ce9fa9bc266097c454f90812f5b903d52dc0fb3a`.
Evidence is in
`/Users/frityet/Scratch/ljmt-byte-snapshots-forl-complete-20261030/`:
`gate-source.txt`, `complete-gate.log`, and `gate-restoration.log`.

The ordinary matrix at `e9ab0f3e0` has identical production and fixtures to
this checkpoint. ARM64 assertions and strict bare-`-Werror` x86_64 assertions
each pass 509 source cases, 509 original v2-bytecode cases, 260 workload
semantic checks, topology and expanded probes, ninety fixed/dynamic-positive
byte rows, forty dynamic-negative byte rows, the raw-U8 live-peer witness
and twelve FFI-index rows. The workload inventory observes native root exits
in eighteen of twenty rows. The U8 probe observes 84 selected-root exits and
unchanged machine code after GC on both architectures. Immutable binaries,
matching JIT modules, archives, hashes and logs are in
`/Users/frityet/Scratch/ljmt-byte-snapshots-forl-ordinary-20261030/`.

Promotion covers this frozen revision. Later mixed-sign byte ranges have
focused validation; substring allocation, string-identity `tostring`, raw U8
multiple-result snapshots and sequential leaf branches remain separate work.
General Lua calls/returns, recursion, protected calls, remaining FFI forms
and broader trace topology still prevent full x86_64 parity.

After promotion, the main checkout independently rebuilt ordinary assertion
ARM64 without test helpers. `main-ordinary-build.log` records success and
profile `6e5be1d551261e02beee167040afc8f593a2bfe34b1d67cb9416825eb8eb8494`.

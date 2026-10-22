# ARM64 FFI aggregate snapshots — 2026-10-22

The interpreter's ARM64 struct classifier still walked raw CType table pointers,
unlike the fork's x86-64 classifier. A concurrent parser can replace or retire
that table. The ARM64 classifier now uses the existing parser-aware snapshot
helpers for fields, array elements, nested structs, and anonymous subtypes.
Its ABI rules, register allocation, and aggregate return handling are unchanged.
No lock or architecture-independent hot-path abstraction was added.

The parent and each traversed field/child remain local snapshots across waits.
The containing field snapshot lives across loop iterations; array traversal
retains the enclosing size before replacing its child snapshot. The current
Lua state is passed through recursive classification so parser waits use the
existing native-state and safepoint protocol.

## Validation

`tools/ci/arm64_ffi_aggregate_contract.sh` links a private instrumented FFI object
against an existing native ARM64 archive, preserving the checked-out executable.
It selects matching interpreter or experimental-JIT layout automatically. Both
ARM64 umbrella gates invoke it. The test-only export is absent from normal builds.

The fixture covers:

- Float HFA with nested arrays, nested double HFA, double unions, complex fields,
  mixed integer/FP aggregates, indirect large results, and GP/FP register overflow.
- Correct return values and errno, including allocation and collection.
- A parent snapshot acquired before a held parser token; field traversal must
  enter the existing native wait and resume after release, for seven ABI classes.
- Three aggregate-call workers alongside 512 newly parsed struct definitions.

An isolated assertion-enabled safe ARM64 build passed the fixture and all 387
vendored stock tests. Substituting the previous classifier into that build
failed at `wait.saw_native`, demonstrating that the wait test detects the old
raw traversal rather than a wait elsewhere in argument preparation.

This change concerns interpreter FFI calls. It does not enable generic JIT
aggregate CALLXS lowering or claim complete FFI concurrency coverage.

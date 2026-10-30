# Integer formatting and child-return validation complete checkpoint

Frozen source `505069708` passes `tools/ci/arm64_jit_fail_closed_gate.sh` on Xcode 27.2 (27B52), clang 21.0.0, MacOSX27.2.sdk. The outer runner reports `gate exit=0; restore exit=0`. All four M7 FFI profiles, actual VM/recorder safepoint runtime checks, the existing ordinary runtime contracts and independent semantic/allocation mutation suites pass. Ordinary assertion ARM64 without test helpers was restored with profile `222c1bf5007a792efc12e6e06caa64715ec7c0304a127c7164d63f9ca2112703`.

Exact source, toolchain, preflight, complete gate and restoration logs are under `/Users/frityet/Scratch/ljmt-int-concat-child-complete-sdk272-20261030/`.

Compared with the preceding `68c122f2a` checkpoint, this adds INT TOSTR through LuaJIT's existing integer-formatting helper and captures an exact bounded child-return certificate for validation. Constant-payload bounds, current side identity, snapshot frame markers and one-use capture have independent negative controls. This groundwork does not enable child-frame side publication. The existing lockless lifecycle and independent admission passes remain in force.

The INT formatting component passed all four profiles, fresh-string publication under the native JIT lease, and matching ordinary ARM64/strict-x86_64 native probes. Earlier combined source `43bd1ca75` passed formatting and leaf contracts across all four profiles. See `arm64-integer-concat-tostr-2026-10-30.md` and `arm64-sequential-child-side-transition-2026-10-30.md` for bounded claims and evidence. The final frozen source additionally includes the validated frame-marker consumer and formatting cleanup.

The later INT modulo, retained-modulo-result, unary-leaf and CHAR formatter changes are excluded from this full gate. This is a passing complete gate for the named source, not full parity with x86_64. Child-frame side publication, broader Lua calls/returns, recursion, protected calls and remaining iterator/FFI restrictions remain open. An independent ordinary rebuild of the promoted main checkout follows.

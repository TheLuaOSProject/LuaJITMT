# Darwin ARM64 callback lifecycle validation

The focused callback lifecycle fixtures are registered as
`m7_ffi_callback_arm64_lifecycle` in `tests/suites/m7_ffi.lua`. The case builds
an assertion-enabled ARM64 bootstrap archive with JIT disabled and runs the
nested-native, owner-lifetime, STOPREQ, attached-carrier, TLS-less auto-attach,
and C++ auto-unwind fixtures. Every C and C++ fixture uses the same profile
macros as the archive because they inspect GC2 state through inline accessors.

On 2026-10-25 the case passed all six fixtures from an isolated copy of
`c3cbb04a`. The separate
`tools/ci/arm64e_ffi_callback_lifecycle_contract.sh` built an assertion-enabled
arm64e/BTI archive and ran the same six fixtures successfully, then restored
the requested ARM64 profile. Each arm64e process has a 30-second alarm deadline;
the ARM64 suite case applies a 30-second runner timeout to every fixture.

An earlier standalone attempt linked a `LUA_USE_ASSERT` archive while compiling
the C++ fixture without that macro. Assertion builds add fields to GC2 state,
so the fixture's inline `gc2_n_threads_acq()` read used a different layout and
reported a garbage count before entering the callback. The suite and arm64e
contract now compile every fixture with matching assertion-enabled flags.
`src/lj_def.h` also keeps `lj_assert_check` void-valued while making both
conditional branches C++-compatible, allowing the C++ fixture to retain the
same assertion layout as the library.

The isolated reruns started from frozen `c3cbb04a` and applied only
`src/lj_def.h`, `tests/suites/m7_ffi.lua`, and
`tools/ci/arm64e_ffi_callback_lifecycle_contract.sh` before rebuilding.

This contract covers the six interpreter/native callback lifecycle fixtures.
It does not establish unrestricted ARM64 JIT callback-stack coverage.

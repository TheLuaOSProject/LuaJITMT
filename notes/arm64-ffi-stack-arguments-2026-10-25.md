# Fixed scalar FFI stack arguments on Darwin ARM64

The experimental ARM64 JIT now admits fixed scalar FFI arguments after either
argument-register bank is exhausted. This extends the existing scalar CALLXS
capability; it does not open aggregate, variadic, vector or arbitrary result
graphs. The current checkout is on `v2.1`.

## Implementation

`lj_ccall_arm64_signature` classifies up to the existing 32-argument limit
using stable CType snapshots. GPR and FPR counts saturate independently at
eight. Overflow arguments record their natural alignment, byte offset and
total unrounded stack extent. Darwin packs fixed stack arguments at their
natural widths, including bytes and halfwords; the existing assembler rounds
the complete stack frame to 16 bytes. Over-aligned stack carriers remain
rejected. This follows Apple's
[ARM64 calling-convention documentation](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms).

Semantic admission checks the CType plan against the actual IR widths and
bank allocation. Post-register-allocation admission independently reconstructs
the bank allocation and stack layout from the allocated operands, checks
the frame capacity, and rejects any spill overlapping the largest outgoing
argument area. It retains the existing snapshot, live-across-call, native-frame
and result-lifetime checks. Narrow stack arguments accept the recorder's exact
INT-to-U8/U16 carrier conversion; their stores consume the low CType-width
bits. Narrow register arguments continue to use extended INT carriers.

The only emitter change masks guard/PHI flags before indexing
`lj_ir_type_size`, in both outgoing-area reservation and stack-store emission.
The old `irt_size` indexing could read beyond that table for a guarded or PHI
loop value passed on the stack. The 13-integer C++ fixture exposed this while
the independent admission check correctly rejected the inconsistent width.
The existing stack argument emitter and recorder lowering otherwise remain
unchanged. No lock, shared runtime policy or x86-64 production code is added.

## Focused evidence

The complete `m7_ffi_callxs_arm64_scalar` suite passed on all four profiles:

| Profile | Log under `/tmp/` |
| --- | --- |
| ARM64 assertions | `ljmt-stack-m7-arm64.log` |
| arm64e/BTI assertions | `ljmt-stack-m7-arm64e.log` |
| ARM64 release | `ljmt-stack-m7-arm64-release.log` |
| arm64e/BTI release | `ljmt-stack-m7-arm64e-release.log` |

The new coverage requires observed ACTIVE, pinned native frames, not merely
trace publication or correct interpreter output:

- Nine integer and nine double arguments exhaust each bank separately.
  An alternating 18-argument call exhausts both banks independently.
- A 21-argument mixed call checks byte, halfword, double, pointer and integer
  stack arguments at offsets 0, 2, 8, 16 and 24 (28 bytes total). Distinct
  emitted stores of all five widths/offsets are required. Live integer and
  double values survive four calls with different outgoing extents per loop.
- Separate dynamic narrow-argument runs check both -7/1234 and 249/66770,
  which produce the same low byte/halfword values. Each run requires at least
  200 additional native calls.
- Authentic trace mutations independently reject widened narrow carriers,
  shortened CARG chains and swapped byte/halfword arguments at both admission
  boundaries. Allocated-state mutations reject an undersized frame and a
  live spill moved into the outgoing area.
- A nine-integer signature exercises callbacks, nested generated calls,
  callback errors, STOPREQ cleanup, errno and immediate trace reuse while
  checking argument integrity around the callback.
- The C++ unwind fixture runs both its original register signature and a
  13-integer signature. The latter places five integers (20 bytes) on the
  stack, beyond the fixed spill area. Each mode observes 1,195 generated
  calls, including an error unwinding through C++/Lua frames, destructor
  cleanup, and native recovery after GC/flush.

The four profiles use matching runtime/archive/fixture/helper-library flags
through the existing shared build helper. The complete experimental gate
already invokes these profiles and therefore includes the new coverage.

## Combined validation

All of the following passed sequentially on clean, frozen source
`effe64673520f03856ce8036ece201c728b13821`. Source did not change during the
combined run, and the checkout was clean afterward. Later changes to this
checkpoint only update documentation.

| Check | Result | Log under `/tmp/` |
| --- | --- | --- |
| Complete experimental gate | All default branches passed, including all four M7 profiles and both C++ unwind signatures in each | `ljmt-stack-experimental-effe6467.log` |
| ARM64 stock source corpus | 509 passed | `ljmt-stack-stock-source-effe6467.log` |
| ARM64 original-v2 bytecode import corpus | 509 passed | `ljmt-stack-stock-import-effe6467.log` |
| Complete bootstrap gate | Passed, including 387 stock cases, threaded tmpbuf, Darwin roots/GC2, ARM64/arm64e callbacks, aggregate/variadic FFI and x86-64 errno unwind | `ljmt-stack-bootstrap-effe6467.log` |
| Development profile restoration | Thin ARM64 assertion runtime, JIT enabled, no test-helper defines | `ljmt-stack-development-profile-effe6467.log` |

Builds used Apple SDK clang/clang++, SDKROOT from `xcrun --sdk macosx`,
deployment target 13.0 and `JOBS=2 MAKE_JOBS=2`. No test branch, assertion or
timeout was disabled. The experimental numeric-argument contract ran all
222 default executions. The final development profile fingerprint is
`a2c50c1dcdd16f500f9873dee8d7094b6f1da122a662327b6d16a762c00caa2e`.

Reproduce serially in one checkout:

```sh
export LJ_TEST_ROOT="$PWD" JOBS=2 MAKE_JOBS=2
export CC="$(xcrun --sdk macosx --find clang)"
export CXX="$(xcrun --sdk macosx --find clang++)"
export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
export MACOSX_DEPLOYMENT_TARGET=13.0
sh tools/ci/arm64_jit_fail_closed_gate.sh
(cd tests/stock/test && ../../../src/luajit test.lua --quiet)
(cd tests/stock/test && LUAJIT_STOCK=/opt/homebrew/bin/luajit ../../../src/luajit ../../../tools/ci/stock_bytecode_corpus.lua)
sh tools/ci/arm64_bootstrap_gate.sh
```

After bootstrap, restore the development runtime explicitly:

```sh
(
  set -eu
  . tools/ci/lib/arm64_profile.sh
  lj_ci_profile_init "$LJ_TEST_ROOT" stack-development-profile
  lj_ci_build_profile arm64 '-arch arm64' '-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT' stack-development
  lj_ci_run_lua 30 -e 'assert(jit.arch == "arm64" and jit.status())'
  LJ_CI_RESTORE_NEEDED=0
)
```

## Remaining scope

The signature classifier recognizes more scalar types than every currently
admitted IR/result graph can express. Unsupported float/boxed-64-bit graphs,
aggregates/sret, variadics, vectors and complex values remain closed. The
new stack-specific tests do not constitute a new remote-concurrency or
sanitizer matrix. The full ARM64 port remains incomplete; the other limits
in [`README.md`](README.md) still apply.

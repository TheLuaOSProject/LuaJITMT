# Target-only ASAN coverage for ARM64 FFI and scalar sides (2026-10-25)

The initial production source checkpoint was `0b7dcb52c378bda46297ddf102d9530b525d37fc`. A separate test-only harness correction, `eaef910eaddfe1a4e05f56a29ec6b17dd6b92cdc`, made M7 shared libraries inherit the selected target and sanitizer profile. The initial private validation HEAD was `41228bd7e503d1098887b0544ce9584562a3f974`. A follow-up ARM64 M7 run used final checkpoint `9b0c6e187d3f131c344f6116297c2fdd4f25072e`, which includes final semantic footer tests. No production source was changed for the follow-up run.

All target code used Apple SDK clang with `MACOSX_DEPLOYMENT_TARGET=13.0`, `JOBS=2`, and target-only instrumentation:

```sh
TARGET_CFLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address'
TARGET_LDFLAGS='-fsanitize=address'
TARGET_SHLDFLAGS='-fsanitize=address'
ASAN_OPTIONS='detect_leaks=0:halt_on_error=1:abort_on_error=1'
```

Host generators used the ordinary unsanitized native Apple clang. The initial ARM64 side archive and runtime both linked `libclang_rt.asan_osx_dynamic.dylib`; `src/host/minilua` was arm64 and had no ASAN dependency. M7 shared libraries were verified with `lipo -archs` and `otool -L` for their matching target architecture and ASAN dependency after the shared-library profile correction. The first ARM64 M7 run preceded that correction and is not counted as proof of instrumented shared libraries; the follow-up run below is.

## Results

- `arm64_jit_side_ir_admission_contract.sh` passed on arm64 and arm64e/BTI; both reported native scalar-side adaptation and guarded refusal verified.
- `arm64_jit_first_side_production_contract.sh` passed its no-helper smoke, repeated helper entry/retirement cases, and restore under ASAN. Its log records separate arm64 and arm64e profiles; all GC-claim, scoped-retirement, and full-flush runs completed.
- `m7_ffi_callxs_arm64_scalar` passed under ASAN on arm64 and arm64e/BTI. This includes semantic and post-RA admission negatives, ABI-class coverage, 1,195 generated C++ callback calls/unwind, remote flush, and integer/double/pointer lifecycle checks.
- No `AddressSanitizer` error report appeared in any run log.

### Final ARM64 M7 shared-library ASAN rerun

At source SHA `9b0c6e187d3f131c344f6116297c2fdd4f25072e`, after the shared-library profile fix, `m7_ffi_callxs_arm64_scalar` passed again on ARM64. Admission and ABI fixtures, the 1,195-call C++ callback/unwind fixture, remote flush, and integer/double/pointer lifecycle fixtures all ran successfully. Each of the six generated shared libraries was `arm64` by `lipo -archs` and linked `libclang_rt.asan_osx_dynamic.dylib` by `otool -L`; `src/luajit` was arm64 and linked the ASAN runtime. The log is `/private/tmp/ljmt-asan-arm64-final-9b0c6e18-tmp/m7-callxs-arm64.log`.

The arm64e profile used `TARGET_FLAGS='-arch arm64e -mbranch-protection=bti'` and `CPPFLAGS='-DLUAJIT_ENABLE_CET_BR'`; the M7 test target identified as arm64 for suite selection. The arm64e M7 helper commit also carries `-DLUAJIT_ENABLE_CET_BR` through fixture and shared-library compilation.

## Commands

The standalone ARM64 side archive used this matching build profile:

```sh
SDKROOT=$(xcrun --sdk macosx --show-sdk-path) MACOSX_DEPLOYMENT_TARGET=13.0 TMPDIR=/private/tmp/ljmt-asan-0b7dcb52-tmp JOBS=2 make -C src clean TARGET_SYS=Darwin TARGET_FLAGS='-arch arm64' TARGET_CFLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address' TARGET_LDFLAGS='-fsanitize=address' TARGET_SHLDFLAGS='-fsanitize=address' XCFLAGS='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS'
SDKROOT=$(xcrun --sdk macosx --show-sdk-path) MACOSX_DEPLOYMENT_TARGET=13.0 TMPDIR=/private/tmp/ljmt-asan-0b7dcb52-tmp JOBS=2 make -C src -j2 TARGET_SYS=Darwin TARGET_FLAGS='-arch arm64' TARGET_CFLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address' TARGET_LDFLAGS='-fsanitize=address' TARGET_SHLDFLAGS='-fsanitize=address' XCFLAGS='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS'
```

The arm64e archive used the same commands with `TARGET_FLAGS='-arch arm64e -mbranch-protection=bti'` and `-DLUAJIT_ENABLE_CET_BR` appended to `XCFLAGS`.

For the scalar-side admission contract, the arm64 profile used:

```sh
TMPDIR=/private/tmp/ljmt-asan-0b7dcb52-tmp JOBS=2 TARGET_CFLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address' TARGET_LDFLAGS='-fsanitize=address' TARGET_SHLDFLAGS='-fsanitize=address' ASAN_OPTIONS='detect_leaks=0:halt_on_error=1:abort_on_error=1' LJ_TEST_XCFLAGS='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS' LJ_TEST_TARGET_SYS=Darwin sh tools/ci/arm64_jit_side_ir_admission_contract.sh
```

The arm64e run used the same command plus `LJ_ARM64_SIDE_ARCH=arm64e` and `-DLUAJIT_ENABLE_CET_BR` appended to `LJ_TEST_XCFLAGS`.

The production side lifecycle contract used the target-only ASAN variables and `ASAN_OPTIONS` above:

```sh
TMPDIR=/private/tmp/ljmt-asan-0b7dcb52-tmp JOBS=2 TARGET_CFLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address' TARGET_LDFLAGS='-fsanitize=address' TARGET_SHLDFLAGS='-fsanitize=address' ASAN_OPTIONS='detect_leaks=0:halt_on_error=1:abort_on_error=1' LJ_TEST_TARGET_SYS=Darwin sh tools/ci/arm64_jit_first_side_production_contract.sh
```

M7 ran via the native ARM64 Homebrew LuaJIT control runner. For arm64e, `TARGET_FLAGS='-arch arm64e -mbranch-protection=bti'`, `CPPFLAGS='-DLUAJIT_ENABLE_CET_BR'`, the Apple SDK clang/clang++, `LJ_TEST_TARGET_ARCH=arm64`, and the ASAN settings above were provided:

```sh
SDKROOT=$(xcrun --sdk macosx --show-sdk-path) MACOSX_DEPLOYMENT_TARGET=13.0 TMPDIR=/private/tmp/ljmt-asan-0b7dcb52-tmp TARGET_SYS=Darwin TARGET_FLAGS='-arch arm64e -mbranch-protection=bti' CPPFLAGS='-DLUAJIT_ENABLE_CET_BR' CC=$(xcrun --sdk macosx --find clang) CXX=$(xcrun --sdk macosx --find clang++) HOST_CC=$(xcrun --sdk macosx --find clang) JOBS=2 TARGET_CFLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address' TARGET_LDFLAGS='-fsanitize=address' TARGET_SHLDFLAGS='-fsanitize=address' ASAN_OPTIONS='detect_leaks=0:halt_on_error=1:abort_on_error=1' LJ_TEST_TARGET_ARCH=arm64 arch -arm64 /opt/homebrew/bin/luajit tools/test.lua m7_ffi_callxs_arm64_scalar
```

The arm64 M7 command used the same line with `TARGET_FLAGS='-arch arm64'` and without the CET `CPPFLAGS` override. Initial side and arm64e run logs are under `/private/tmp/ljmt-asan-0b7dcb52-tmp/`: `side-arm64.log`, `side-arm64e.log`, `side-lifecycle.log`, and `m7-callxs-arm64e.log`. The definitive ARM64 M7 rerun log is `/private/tmp/ljmt-asan-arm64-final-9b0c6e18-tmp/m7-callxs-arm64.log`; the initial pre-fix ARM64 M7 log is not used as evidence of ASAN-instrumented shared libraries.

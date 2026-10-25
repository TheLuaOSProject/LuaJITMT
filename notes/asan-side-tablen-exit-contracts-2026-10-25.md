# Target-only ASAN side-admission and rooted-length contracts (2026-10-25)

Validation used a clean detached clone at production checkpoint `241a41e858fef64fd949bf83a4525e12bbb13113`. No source files were changed. Builds used Apple SDK clang, `MACOSX_DEPLOYMENT_TARGET=13.0`, `JOBS=2`, and target-only ASAN flags:

```sh
TARGET_CFLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address'
TARGET_LDFLAGS='-fsanitize=address'
TARGET_SHLDFLAGS='-fsanitize=address'
ASAN_OPTIONS='detect_leaks=0:halt_on_error=1:abort_on_error=1'
```

The leak setting remains disabled for these process-wide runtime fixtures. Host generators were not instrumented: `src/host/minilua` was native arm64 and had no ASAN dependency. The target runtime linked `libclang_rt.asan_osx_dynamic.dylib`; the final arm64e build was a thin arm64e executable and archive.

## Results

- `arm64_jit_rooted_tablen_contract.sh` passed with separate ASAN archive/fixture profiles for ARM64 and arm64e/BTI. It covered semantic and post-RA admission, native rooted-helper execution, STOPREQ unwind/resume, and rooted full-GC lifecycle. Profile IDs from the log: ARM64 `b78f665e6d7971e56faecc0cc5e87aad647f6800856acca5f9ada01272252950`; arm64e `40c1398a5af807b54bf45d3b75dae1db9e84cfa2b4beabf169a6917f5eda54c4`.
- `arm64_jit_side_ir_admission_contract.sh` passed adaptation and guarded-refusal checks on ARM64 and arm64e/BTI. Each run bound its fixture to the matching ASAN archive profile. Profile IDs: ARM64 `4b467621aeb3bff8b95d0afce8acd3ea3d448e03e800b01409c8630c9fdfbd0d`; arm64e `e54ee42d7b1a59dde7ca69d85aaa14d80b1b05737dbc3e5bf7a8e96e4355c286`.
- No ASAN error report appeared in any log.

The side-admission profiles used `XCFLAGS='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS'`; the arm64e profile added `-DLUAJIT_ENABLE_CET_BR` and used `TARGET_FLAGS='-arch arm64e -mbranch-protection=bti'`. Rooted-length profiles additionally enabled `-DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS`.

## Reproduction

The rooted-length contract builds and runs both target profiles in one invocation:

```sh
TMPDIR=/private/tmp/ljmt-asan-side-tablen-241a41e8-tmp JOBS=2 TARGET_CFLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address' TARGET_LDFLAGS='-fsanitize=address' TARGET_SHLDFLAGS='-fsanitize=address' ASAN_OPTIONS='detect_leaks=0:halt_on_error=1:abort_on_error=1' LJ_TEST_TARGET_SYS=Darwin sh tools/ci/arm64_jit_rooted_tablen_contract.sh
```

For side admission, build the matching archive first, then run the contract against it. The ARM64 build and run were:

```sh
SDKROOT=$(xcrun --sdk macosx --show-sdk-path) MACOSX_DEPLOYMENT_TARGET=13.0 TMPDIR=/private/tmp/ljmt-asan-side-tablen-241a41e8-tmp JOBS=2 make -C src clean TARGET_SYS=Darwin TARGET_FLAGS='-arch arm64' TARGET_CFLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address' TARGET_LDFLAGS='-fsanitize=address' TARGET_SHLDFLAGS='-fsanitize=address' XCFLAGS='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS'
SDKROOT=$(xcrun --sdk macosx --show-sdk-path) MACOSX_DEPLOYMENT_TARGET=13.0 TMPDIR=/private/tmp/ljmt-asan-side-tablen-241a41e8-tmp JOBS=2 make -C src -j2 TARGET_SYS=Darwin TARGET_FLAGS='-arch arm64' TARGET_CFLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address' TARGET_LDFLAGS='-fsanitize=address' TARGET_SHLDFLAGS='-fsanitize=address' XCFLAGS='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS'
TMPDIR=/private/tmp/ljmt-asan-side-tablen-241a41e8-tmp JOBS=2 TARGET_CFLAGS='-O1 -g -fno-omit-frame-pointer -fsanitize=address' TARGET_LDFLAGS='-fsanitize=address' TARGET_SHLDFLAGS='-fsanitize=address' ASAN_OPTIONS='detect_leaks=0:halt_on_error=1:abort_on_error=1' LJ_TEST_XCFLAGS='-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_TRACE_TEST_HELPERS' LJ_TEST_TARGET_SYS=Darwin sh tools/ci/arm64_jit_side_ir_admission_contract.sh
```

For arm64e, use `TARGET_FLAGS='-arch arm64e -mbranch-protection=bti'`, append `-DLUAJIT_ENABLE_CET_BR` to `XCFLAGS`, set `LJ_ARM64_SIDE_ARCH=arm64e`, and append the same define to `LJ_TEST_XCFLAGS` on the final command.

Logs are under `/private/tmp/ljmt-asan-side-tablen-241a41e8-tmp/`: `rooted-tablen.log`, `build-side-arm64.log`, `side-admission-arm64.log`, `build-side-arm64e.log`, and `side-admission-arm64e.log`.

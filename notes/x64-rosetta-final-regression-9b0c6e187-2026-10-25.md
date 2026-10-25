# x86-64/Rosetta final shared-runtime regression

Validation used a fresh detached clone at `/private/tmp/ljmt-x64-final-9b0c6e18`,
source SHA `9b0c6e187d3f131c344f6116297c2fdd4f25072e`. The worktree was clean
before adding this evidence note. The host reported `arm64`; under
`arch -x86_64`, `sysctl.proc_translated` returned `1`.

The target runtime and fixtures used Apple SDK clang, deployment target 13.0,
`TARGET_SYS=Darwin`, `TARGET_FLAGS='-arch x86_64 -mcx16'`, and
`XCFLAGS='-DLUA_USE_ASSERT'`. `HOST_CC=clang` kept build tools native ARM64,
`JOBS=2` limited target builds, and `LDFLAGS=-Wl,-adhoc_codesign` signed target
links. The runner was native `/opt/homebrew/bin/luajit` under `arch -arm64`;
target processes ran under Rosetta.

## Results

- Thin x86_64 runtime build passed. `lipo -info src/luajit` reported only
  `x86_64`; `codesign -dv --verbose=2 src/luajit` reported `adhoc,linker-signed`
  and `Signature=adhoc`.
- Vendored stock source suite: **509 passed**.
- Stock bytecode import corpus: **509 passed**.
- `m7_ffi_errno_preservation`: passed.
- `m7_ffi_callxs_authentic`: passed production XSAVE/native/CALLXS execution,
  remote flush, post-call/STOPREQ unwind, and generated callback suspension,
  nesting, and unwind.
- `m7_ffi_callxs_callback_stack`: passed all three modes: 80 generated
  callbacks without relocation, 80 with stack relocation, and the throw path.
- `lipo` and `codesign -dv` checks confirmed the errno fixture, post-call
  fixture, generated-callback fixture, callback-stack fixture, and both CALLXS
  shared libraries were thin x86_64 and ad-hoc signed.

## Commands

```sh
MACOSX_DEPLOYMENT_TARGET=13.0 TMPDIR=/private/tmp/ljmt-x64-final-9b0c6e18-tmp HOST_CC=clang LDFLAGS=-Wl,-adhoc_codesign make -C src clean TARGET_SYS=Darwin TARGET_FLAGS='-arch x86_64 -mcx16' XCFLAGS='-DLUA_USE_ASSERT'
MACOSX_DEPLOYMENT_TARGET=13.0 TMPDIR=/private/tmp/ljmt-x64-final-9b0c6e18-tmp HOST_CC=clang LDFLAGS=-Wl,-adhoc_codesign JOBS=2 make -C src -j2 TARGET_SYS=Darwin TARGET_FLAGS='-arch x86_64 -mcx16' XCFLAGS='-DLUA_USE_ASSERT'
lipo -info src/luajit
codesign -dv --verbose=2 src/luajit
```

```sh
cd tests/stock/test
TMPDIR=/private/tmp/ljmt-x64-final-9b0c6e18-tmp arch -x86_64 ../../../src/luajit test.lua --quiet
TMPDIR=/private/tmp/ljmt-x64-final-9b0c6e18-tmp LUAJIT_STOCK=/opt/homebrew/bin/luajit arch -x86_64 ../../../src/luajit ../../../tools/ci/stock_bytecode_corpus.lua
```

```sh
MACOSX_DEPLOYMENT_TARGET=13.0 TMPDIR=/private/tmp/ljmt-x64-final-9b0c6e18-tmp TARGET_SYS=Darwin TARGET_FLAGS='-arch x86_64 -mcx16' CC='clang -arch x86_64 -mcx16' HOST_CC=clang LDFLAGS=-Wl,-adhoc_codesign LJ_TEST_TARGET_ARCH=x64 XCFLAGS='-DLUA_USE_ASSERT' arch -arm64 /opt/homebrew/bin/luajit tools/test.lua m7_ffi_errno_preservation
MACOSX_DEPLOYMENT_TARGET=13.0 TMPDIR=/private/tmp/ljmt-x64-final-9b0c6e18-tmp TARGET_SYS=Darwin TARGET_FLAGS='-arch x86_64 -mcx16' CC='clang -arch x86_64 -mcx16' HOST_CC=clang LDFLAGS=-Wl,-adhoc_codesign LJ_TEST_TARGET_ARCH=x64 XCFLAGS='-DLUA_USE_ASSERT' arch -arm64 /opt/homebrew/bin/luajit tools/test.lua m7_ffi_callxs_authentic m7_ffi_callxs_callback_stack
```

Logs are in `/private/tmp/ljmt-x64-final-9b0c6e18-tmp/`: `build.log`,
`signing.log`, `stock-source.log`, `stock-import.log`, `errno.log`, and
`callxs-callback.log`. These are x86_64/Rosetta regression results for the
listed suites and do not claim a full M7 or umbrella run.

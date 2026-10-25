# x86-64/Rosetta XSAVE literal regression at 815e7d5e

Validation used a clean detached clone at `/private/tmp/ljmt-final-x64-xsavesmoke-815e7d5e`, frozen at `815e7d5e8a1410e52b47e9358c31dbea1cc11d8b`. The production delta from the prior x64 checkpoint is the `IR_XSAVE` early path in `lj_opt_fold.c`, which preserves ordered raw literal halves before fold-key construction. No production files were changed in this validation clone.

All target builds used the Apple SDK clang, `MACOSX_DEPLOYMENT_TARGET=13.0`, `TARGET_SYS=Darwin`, `TARGET_FLAGS='-arch x86_64 -mcx16'`, and `XCFLAGS='-DLUA_USE_ASSERT'`. `HOST_CC=clang` kept build-time tools native ARM64. `LDFLAGS=-Wl,-adhoc_codesign` signed linked x86-64 executables at link time; the main `src/luajit` was also verified as thin x86_64 and linker ad-hoc signed. The test runner itself used the native ARM64 Homebrew LuaJIT, while target executables ran under Rosetta. `JOBS=2` was used for builds. The private temp/log directory was `/private/tmp/ljmt-x64-xsavesmoke-815e7d5e-tmp`.

## Results

- Vendored stock source suite: **509 passed**.
- Stock bytecode import corpus: **509 passed**.
- Registered `m7_ffi_errno_preservation`: passed.
- `m7_ffi_callxs_authentic`: passed production XSAVE/CALLXS execution, remote flush, exact post-call/STOPREQ unwind, and generated callback suspend/nest/unwind checks.
- `m7_ffi_callxs_callback_stack`: passed all three modes, including 80 generated callbacks with stack relocation and the throw/unwind path.

The focused XSAVE/CALLXS suites therefore exercised generated x86-64 native code and callback stack behavior, not only source or build checks.

## Reproduction

From the detached clone, the archive build was:

```sh
MACOSX_DEPLOYMENT_TARGET=13.0 TMPDIR=/private/tmp/ljmt-x64-xsavesmoke-815e7d5e-tmp HOST_CC=clang LDFLAGS=-Wl,-adhoc_codesign make -C src clean TARGET_SYS=Darwin TARGET_FLAGS='-arch x86_64 -mcx16' XCFLAGS='-DLUA_USE_ASSERT'
MACOSX_DEPLOYMENT_TARGET=13.0 TMPDIR=/private/tmp/ljmt-x64-xsavesmoke-815e7d5e-tmp HOST_CC=clang LDFLAGS=-Wl,-adhoc_codesign JOBS=2 make -C src -j2 TARGET_SYS=Darwin TARGET_FLAGS='-arch x86_64 -mcx16' XCFLAGS='-DLUA_USE_ASSERT'
```

The two stock checks were:

```sh
(cd tests/stock/test && TMPDIR=/private/tmp/ljmt-x64-xsavesmoke-815e7d5e-tmp arch -x86_64 ../../../src/luajit test.lua --quiet)
(cd tests/stock/test && TMPDIR=/private/tmp/ljmt-x64-xsavesmoke-815e7d5e-tmp LUAJIT_STOCK=/opt/homebrew/bin/luajit arch -x86_64 ../../../src/luajit ../../../tools/ci/stock_bytecode_corpus.lua)
```

The errno fixture and authentic CALLXS/callback-stack cases used the registered harness under the native ARM64 runner, with matching target profile:

```sh
MACOSX_DEPLOYMENT_TARGET=13.0 TMPDIR=/private/tmp/ljmt-x64-xsavesmoke-815e7d5e-tmp TARGET_SYS=Darwin TARGET_FLAGS='-arch x86_64 -mcx16' CC='clang -arch x86_64 -mcx16' HOST_CC=clang LDFLAGS=-Wl,-adhoc_codesign LJ_TEST_TARGET_ARCH=x64 XCFLAGS='-DLUA_USE_ASSERT' arch -arm64 /opt/homebrew/bin/luajit tools/test.lua m7_ffi_errno_preservation
MACOSX_DEPLOYMENT_TARGET=13.0 TMPDIR=/private/tmp/ljmt-x64-xsavesmoke-815e7d5e-tmp TARGET_SYS=Darwin TARGET_FLAGS='-arch x86_64 -mcx16' CC='clang -arch x86_64 -mcx16' HOST_CC=clang LDFLAGS=-Wl,-adhoc_codesign LJ_TEST_TARGET_ARCH=x64 XCFLAGS='-DLUA_USE_ASSERT' arch -arm64 /opt/homebrew/bin/luajit tools/test.lua m7_ffi_callxs_authentic m7_ffi_callxs_callback_stack
```

Logs: `build.log`, `stock-source.log`, `stock-import.log`, `errno.log`, and `callxs-callback.log` under the private temp directory above. The errno suite ended with `ok m7_ffi_errno_preservation` and `M7 FFI errno preservation fixtures passed`.

# x86-64/Rosetta regression check at 47ba8734 (2026-10-25)

This validation used a fresh detached worktree at
`47ba8734ef106fecece751fd94969f57b54ab8f1`:
`/private/tmp/ljmt-final-x64-47ba`. No production source was changed. Builds
used the Apple SDK compiler returned by `xcrun --find clang`, macOS SDKROOT,
`MACOSX_DEPLOYMENT_TARGET=13.0`, `TARGET_SYS=Darwin`, and
`TARGET_FLAGS='-arch x86_64 -mcx16'`. Every executed x86-64 binary was
ad-hoc-signed and launched with `arch -x86_64`.

## Passed checks

- The assertion-only archive (`XCFLAGS='-DLUA_USE_ASSERT'`) ran the vendored
  stock source suite and the stock-bytecode import corpus: **509 passed** in
  each. Logs: `/tmp/ljmt-final-x64-stock-source-47ba.log` and
  `/tmp/ljmt-final-x64-stock-import-47ba.log`.
- The registered `m7_ffi_errno_preservation` case passed with
  `TARGET_SYS=Darwin`, `TARGET_FLAGS='-arch x86_64 -mcx16'`, and
  `LJ_TEST_TARGET_ARCH=x64`. Log:
  `/tmp/ljmt-final-x64-errno-47ba.log`.
- `sh tools/ci/x64_atomic_contract.sh` passed using Apple SDK clang. Both
  fixtures executed under Rosetta; object checks confirmed inline
  `CMPXCHG16B` and no atomic runtime imports. Log:
  `/tmp/ljmt-final-x64-atomic-47ba.log`.
- `tests/t-lockless-progress-metrics.c` passed as a direct C-API fixture,
  compiled against the same archive profile and flags
  `-DLUA_USE_ASSERT -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS
  -DLUAJIT_DISABLE_JIT`. It observed 1,001 scalar attempts (1,000 hits, one
  refusal), zero wait calls, restored publisher/rescan credits, and 40,000
  balanced contended credit additions/subtractions. This is instrumented
  Rosetta execution, not release performance evidence. Log:
  `/tmp/ljmt-final-x64-progress-47ba.log`.

## Unresolved suite invocation

The combined registered invocation
`tools/test.lua m3_gc2_table_store_guard m6_jit_hotcount_generation` was
started with the target architecture variables above. It produced no output
and remained in macOS process state `U` for over two minutes. Its process was
PID 3651; after sending TERM and KILL it remained in `U`. `lsof` showed the
x86-64 LuaJIT executable, dyld, and Rosetta runtime, with no child process and
an empty log (`/tmp/ljmt-final-x64-universe-foreign-47ba.log`). A `sample`
attempt also blocked waiting on the process and was terminated. The result
does not establish which suite case or stage stalled, so no nested-universe
table-store or foreign-actor hotcount pass is claimed here. No retry was
started while PID 3651 remained active.

Build logs are `/tmp/ljmt-final-x64-build-47ba.log`,
`/tmp/ljmt-final-x64-helper-build-47ba.log`, and
`/tmp/ljmt-final-x64-progress-build-47ba.log`.

# x86-64/Rosetta shared-runtime regression check (2026-10-25)

## Combined final-code validation

Frozen `d8f02a81081ac6f678279a2dd02e8a959f620ad2` was validated again in
the detached checkout `/private/tmp/lj-x64-d8f02a81`. This includes the rooted
numeric-read and entry-scan changes as well as the shared runtime fixes below.
Both stock corpora passed all 509 cases. The registered
`m7_ffi_errno_preservation` case, `t-gc2-table-store-guard`, and
`t-jit-hotcount-generation` also passed. The latter two retain the
nested-universe ownership and foreign-actor rejection checks.

The assertion build used `MACOSX_DEPLOYMENT_TARGET=13.0`,
`TARGET_SYS=Darwin`, `TARGET_FLAGS='-arch x86_64 -mcx16'`, and
`XCFLAGS='-DLUA_USE_ASSERT'`. The two internal C fixtures used a matching
runtime and translation-unit profile with `LJ_GC2_TEST_HELPERS` and
`LJ_TAB_TEST_HELPERS`. All x86-64 executables were ad-hoc signed before
execution under Rosetta.

For the registered errno case, inherit `TARGET_SYS` and `TARGET_FLAGS` in
addition to `CC='clang -arch x86_64 -mcx16'` and `LJ_TEST_TARGET_ARCH=x64`.
An initial invocation with only the latter two variables failed at the
architecture guard during its internal rebuild. Passing the explicit target
variables fixed the invocation; no source change or test waiver was needed.

## Earlier investigation and correction

Validation used an isolated `git archive` copy of main HEAD `b702e2dc` at `/tmp/luajitmt-x64-shared-runtime-1792922668`. The initial stock compatibility checks used an isolated archive of that HEAD. The subsequent fix was then tested in a fresh isolated archive with the changed safepoint source and regression fixture. No main-tree binaries were rebuilt. All x86-64 executables launched under Rosetta were ad-hoc signed first because unsigned x86-64 processes have hung on this host.

## Build profile

```sh
env MACOSX_DEPLOYMENT_TARGET=13.0 make -C /tmp/luajitmt-x64-shared-runtime-1792922668/src clean \
  TARGET_SYS=Darwin TARGET_FLAGS='-arch x86_64' XCFLAGS='-DLUA_USE_ASSERT'
env MACOSX_DEPLOYMENT_TARGET=13.0 make -C /tmp/luajitmt-x64-shared-runtime-1792922668/src -j2 \
  TARGET_SYS=Darwin TARGET_FLAGS='-arch x86_64' XCFLAGS='-DLUA_USE_ASSERT'
codesign --sign - /tmp/luajitmt-x64-shared-runtime-1792922668/src/luajit
```

The executable was confirmed as Mach-O x86_64 and ad-hoc signed.

## Stock compatibility

From `tests/stock/test` in the isolated copy:

```sh
../../../src/luajit test.lua --quiet
LUAJIT_STOCK=/opt/homebrew/bin/luajit ../../../src/luajit \
  ../../../tools/ci/stock_bytecode_corpus.lua
```

Both passed: **509 passed**. Logs: `/tmp/luajitmt-x64-stock-source-suite.log` and `/tmp/luajitmt-x64-stock-import-corpus.log`.

## GC2 close helper fixture

The private tree was rebuilt with a matched assertion/helper profile:

```sh
env MACOSX_DEPLOYMENT_TARGET=13.0 make -C /tmp/luajitmt-x64-shared-runtime-1792922668/src clean \
  TARGET_SYS=Darwin TARGET_FLAGS='-arch x86_64' \
  XCFLAGS='-DLUA_USE_ASSERT -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS'
env MACOSX_DEPLOYMENT_TARGET=13.0 make -C /tmp/luajitmt-x64-shared-runtime-1792922668/src -j2 \
  TARGET_SYS=Darwin TARGET_FLAGS='-arch x86_64' \
  XCFLAGS='-DLUA_USE_ASSERT -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS'
clang -arch x86_64 -std=gnu11 -O2 -Wall -Wextra -Werror -mcx16 \
  -DLUA_USE_ASSERT -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS \
  -I/tmp/luajitmt-x64-shared-runtime-1792922668/src \
  /tmp/luajitmt-x64-shared-runtime-1792922668/tests/t-gc2-table-store-guard.c \
  /tmp/luajitmt-x64-shared-runtime-1792922668/src/libluajit.a -lm -pthread \
  -o /tmp/luajitmt-x64-shared-runtime-1792922668/tests/t-gc2-table-store-guard
codesign --sign - /tmp/luajitmt-x64-shared-runtime-1792922668/tests/t-gc2-table-store-guard
```

The first matched-helper run stalled during `test_second_universe_public_api_stores`, at `luaL_openlibs(L2)`. Instrumentation isolated the path: JIT enable publishes `RESET_HOTCOUNT|REDISPATCH`; the handshake counts the second universe's main TG, but `safepoint_ack_tg` compared that TG to raw TLS. Nested universes deliberately preserve the outer raw TLS TG, so the second universe's own counted ack was rejected and requeued forever. The negative-control run with the old raw-TLS check stalled; diagnostics are in `/tmp/x64-gc2-handshake.log` and `/tmp/x64-gc2-dispatch-hang2.log`.

The narrow correction in `lj_safepoint.c` uses `lj_thr_get_tg_fallback(g)` for both reset-hotcount owner checks. This resolves the logical owner by global universe while still validating the current physical actor. The second-universe fixture now also asserts that JIT initialization advanced and applied the matching hotcount generation while the outer TLS binding remains unchanged. With this fix, the matched assertion/helper `t-gc2-table-store-guard` passed five consecutive x64 runs; the existing foreign-actor requeue coverage in `t-jit-hotcount-generation` also passed. Both 509-case stock checks were rerun from a clean isolated copy containing the fix and passed.

## Native ARM64 experimental focused regression

A separate isolated copy used the same `lj_safepoint.c` and focused fixture, built with the ARM64 experimental profile (`-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT -DLJ_GC2_TEST_HELPERS -DLJ_TAB_TEST_HELPERS`, `TARGET_SYS=Darwin TARGET_FLAGS='-arch arm64'`). The fixture was compiled with the same defines and run four times; all runs passed. The fixture's new assertion verifies that second-universe `luaL_openlibs` advances and applies the exact JIT hotcount generation while `lj_thr_get_tg()` remains bound to the outer universe. The existing `t-jit-hotcount-generation` test passed on x64 and retains its assertion that a foreign TG-only actor cannot apply the generation.

The follow-up safe ARM64 bootstrap profile (`LUAJIT_MT_ARM64_BOOTSTRAP`, `LUAJIT_DISABLE_JIT`, assertions, and both helper defines) also built and passed the focused fixture after the hotcount assertions were wrapped in `#if LJ_HASJIT`. `tools/ci/jit_hotcount_generation_contract.sh` now checks both fallback-aware safepoint paths and verifies that `lj_thr_get_tg_fallback` validates the current actor against the selected TG; the focused contract passed.

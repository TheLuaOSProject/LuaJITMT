# Completed architecture-review validation

The review implementation is integrated on `codex/aarch64-macos-port`.
Final combined validation ran sequentially on clean, frozen source
`be031e3b92c7484efcae97974d90ff7d9965bd8d`. The source was unchanged throughout
the run, and the working tree was clean after all fixtures and build locks
were removed. Subsequent checkpoint changes update documentation only.

## Final combined results

| Check | Result | Log under `/tmp/` |
| --- | --- | --- |
| Complete experimental gate | Passed all default branches, including independent admission negatives, rooted effects, scalar roots/sides, publication/retirement, entry/flush, and callback lifecycle | `ljmt-review-experimental-final-be031e3b.log` |
| Generic FFI M7 within that gate | Four passes: ARM64, arm64e/BTI, ARM64 release and arm64e/BTI release; each includes 1,195 generated C++/Lua callback unwind calls and recovery | Same experimental log |
| ARM64 stock source corpus | 509 passed | `ljmt-review-stock-source-final-be031e3b.log` |
| ARM64 original-bytecode import corpus | 509 passed | `ljmt-review-stock-import-final-be031e3b.log` |
| Complete bootstrap gate | Passed all default branches, including 387 applicable stock cases, threaded tmpbuf, Darwin roots/GC2, ARM64/arm64e callbacks, aggregate/variadic FFI and x86-64 errno unwind | `ljmt-review-bootstrap-final-be031e3b.log` |
| Restored development profile | Thin ARM64 experimental assertion runtime; `jit.arch == "arm64"` and JIT enabled | `ljmt-review-development-profile-final-be031e3b.log` |

Builds used Apple SDK clang/clang++, SDKROOT from `xcrun --sdk macosx`,
deployment target 13.0, and `JOBS=2 MAKE_JOBS=2`. Target profiles and fixture
identity were checked by the shared build plumbing. No target branch was
disabled to obtain this combined pass.

The development checkout is restored to
`-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT`,
without the test-helper defines used by the gate. Its recorded profile is
`f64452c75d5c7ff44180a0a5c39ac407b7e7e09834218455d12ba0c6bcf0f197`.

## Reproduction

These commands must run serially in one checkout:

```sh
cd /Users/frityet/Projects/LuaJITMT
export LJ_TEST_ROOT="$PWD" JOBS=2 MAKE_JOBS=2
export CC="$(xcrun --sdk macosx --find clang)"
export CXX="$(xcrun --sdk macosx --find clang++)"
export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
sh tools/ci/arm64_jit_fail_closed_gate.sh
(cd tests/stock/test && ../../../src/luajit test.lua --quiet)
(cd tests/stock/test && LUAJIT_STOCK=/opt/homebrew/bin/luajit ../../../src/luajit ../../../tools/ci/stock_bytecode_corpus.lua)
sh tools/ci/arm64_bootstrap_gate.sh
```

The bootstrap command leaves an interpreter profile. Restore the development
profile explicitly afterward:

```sh
(
  set -eu
  . tools/ci/lib/arm64_profile.sh
  lj_ci_profile_init "$LJ_TEST_ROOT" final-development-profile
  lj_ci_build_profile arm64 '-arch arm64' '-DLUAJIT_MT_ARM64_BOOTSTRAP -DLUAJIT_MT_ARM64_JIT_EXPERIMENTAL -DLUA_USE_ASSERT' final-development
  lj_ci_run_lua 30 -e 'assert(jit.arch == "arm64" and jit.status())'
  LJ_CI_RESTORE_NEEDED=0
)
```

## Scope of the supporting evidence

The earlier x86-64/Rosetta checkpoint at `9b0c6e18` passed both 509-case
corpora, errno, authentic generated CALLXS/remote-flush/callback tests and
callback-stack relocation/unwind. Later production changes are ARM64-only.
This is targeted translated-x86 regression evidence, not a native Intel
performance measurement or a complete x86 umbrella run. See
[`x64-rosetta-final-regression-9b0c6e187-2026-10-25.md`](x64-rosetta-final-regression-9b0c6e187-2026-10-25.md).

ASAN, UBSan, bounded TSan drivers, concurrency stress and matching release
measurements have their own explicit checkpoints and limits in the
[`implementation ledger`](review-implementation-2026-10-25.md). They were not
all rerun at `be031e3b`. The latest ASAN side/rooted-length run is documented
in [`asan-side-tablen-exit-contracts-2026-10-25.md`](asan-side-tablen-exit-contracts-2026-10-25.md).

The pure numeric continuation regression requires a real child edge and 100
successful handoffs with 100 exits attributed to the child. The mixed case
also permits a child returning to its now-compatible root, whose terminal
exit is then required. Fresh argument-conversion variants may use either a
validated scalar root or a child; the dedicated division variant explicitly
requires a child exit. A published trace or root-entry counter alone is not
treated as proof that all of a workload ran natively.

ARM64 remains opt-in and capability-bounded. Raw heap IR, heap-effect children,
side chains/stitches, and unproved FFI stack/aggregate/variadic/vector/complex
JIT classes remain closed. Interpreter FFI coverage is separate. These tests
do not prove all interleavings race-free, whole-VM wait-freedom, unrestricted
LuaJIT backend parity, or general release-speed improvements.

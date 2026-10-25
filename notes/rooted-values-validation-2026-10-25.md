# Rooted values combined validation

The complete sequential regression passed on clean, frozen source
`65068cdfe948a6c29c4303dfe97777d9604c4712`. This includes GC-valued rooted
reads and existing-key stores, scalar results through GC identity keys, the
separate FFI lookup grammar and the shared raw nil-deletion correction.

| Check | Result | Log under `/tmp/` |
| --- | --- | --- |
| Complete experimental gate | All default branches passed, including ARM64/arm64e assertion/release rooted GC and four M7 FFI profiles, plus signed x86-64 callback-result lifetime | `ljmt-rooted-experimental-65068cdf.log` |
| ARM64 stock source | 509 passed | `ljmt-rooted-stock-source-65068cdf.log` |
| ARM64 original-v2 bytecode import | 509 passed | `ljmt-rooted-stock-import-65068cdf.log` |
| Signed x86-64 execution | Rooted scalar/GC stores, raw nil-deletion and both 509-case stock corpora passed | `ljmt-rooted-x64-regression-65068cdf.log` |
| Complete bootstrap gate | 387 applicable stock cases, threaded tmpbuf, Darwin roots/GC2, ARM64/arm64e callbacks, aggregate/variadic FFI and x86-64 errno unwind passed | `ljmt-rooted-bootstrap-65068cdf.log` |
| Development restoration | Thin ARM64 assertion runtime, JIT enabled, no test-helper defines | `ljmt-rooted-development-profile-65068cdf.log` |

The serial driver is `/tmp/ljmt-rooted-followup-full.sh`. It runs
`tools/ci/arm64_jit_fail_closed_gate.sh`, both stock corpora, the x86-64 profile
and fixtures, `tools/ci/arm64_bootstrap_gate.sh`, then restores the development
profile. It checks unchanged HEAD and a clean worktree before and after the
run. No source changes, branch skips or timeout overrides occurred during it.

Builds used Apple SDK clang/clang++, SDKROOT from `xcrun --sdk macosx`,
deployment target 13.0, `JOBS=2 MAKE_JOBS=2` and
`LDFLAGS=-Wl,-adhoc_codesign`. Explicit linker signing lets the x86-64 runtime
start on this host. Its assertion profile adds `LJ_TAB_TEST_HELPERS` and
`LJ_TG_ROOT_TEST_HELPERS`; its fingerprint is
`0877736c6b32d239d2fb0b8bd7fd8ff395df3da58d65cb040e7691d36b9cb7b2`.
The final native ARM64 development fingerprint is
`4945d21a5e621fb31fe45761d9a18cb16d177a72d8def5dbd8f67d1cbe6611ca`.

These checks establish the listed native and compatibility regressions at
this source checkpoint. They do not add sanitizer or performance evidence,
complete the x86-64 umbrella, or establish general ARM64 JIT/FFI admission.
The broader limits in [`README.md`](README.md) remain active.

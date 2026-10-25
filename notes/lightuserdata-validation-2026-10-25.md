# Lightuserdata combined validation

The runtime implementation at
`7f377f51e48daa6f40c2ffbafd4ae49e0b6097bc` passed the regression matrix below.
The experimental, stock and x86-64 stages ran on clean, frozen source. The
bootstrap stage needed a test-profile correction described below, then its
complete rerun passed without runtime source changes. This checkpoint combines exact
lightuserdata keys/results in the bounded ARM64 JIT with atomic publication
of the shared lightuserdata segment map. It includes every earlier rooted
scalar/GC read, store and raw nil-deletion regression.

| Check | Result | Log under `/tmp/` |
| --- | --- | --- |
| Complete experimental gate | All default branches, ARM64/arm64e assertion/release rooted-value checks, four M7 FFI profiles and signed x86-64 callback-result lifetime passed | `ljmt-lightuserdata-experimental-7f377f51.log` |
| ARM64 stock source | 509 passed | `ljmt-lightuserdata-stock-source-7f377f51.log` |
| ARM64 original-v2 bytecode import | 509 passed | `ljmt-lightuserdata-stock-import-7f377f51.log` |
| Signed x86-64 execution | Rooted stores, raw nil-deletion, segment-map capacity/CAS/decode races and both 509-case stock corpora passed | `ljmt-lightuserdata-x64-regression-7f377f51.log` |
| Complete bootstrap gate | 387 applicable stock cases, the new M5 segment-map fixture, threaded tmpbuf, Darwin roots/GC2, callbacks, aggregate/variadic interpreter FFI and x86-64 errno unwind passed | `ljmt-lightuserdata-bootstrap-rerun.log` |
| Development restoration | Thin ARM64 assertion runtime, JIT enabled, no test-helper defines | `ljmt-lightuserdata-development-profile-rerun.log` |

The initial serial driver is `/tmp/ljmt-lightuserdata-full.sh`. Its bootstrap
stage stopped at the new `m5_lightuserdata_segment_map` registration: the
fixture rebuilt with only assertion/userdata-helper flags, dropping the
mandatory ARM64 opt-in define. The target guard rejected that build, and the
fixture's default restoration had the same missing profile. This was a
harness configuration failure, not a runtime test pass.

The M5 registration now explicitly composes and restores the selected ARM64
bootstrap or experimental mode. Its userdata helper flag remains confined to
the fixture build. `/tmp/ljmt-lightuserdata-bootstrap-rerun.sh` reran the
complete bootstrap gate and restored the development profile. The only
tracked change during that rerun was the M5 profile correction; no runtime
source changes, branch skips or timeout overrides were involved. Builds used Apple SDK
clang/clang++, SDKROOT from `xcrun --sdk macosx`, deployment target 13.0,
`JOBS=2 MAKE_JOBS=2` and `LDFLAGS=-Wl,-adhoc_codesign`.

The signed x86-64 assertion profile includes table, TG-root and userdata test
helpers; its fingerprint is
`25221dc39a8109167c224ca924c0845d4e0bfbef6a36300f0769df02ae89f140`.
The final ARM64 development profile fingerprint is
`8cffdb004d25be2e2a40113f0425c4049afb87a395a3002cd77517e9fb56f6aa`.
The x86-64 symbol check found no imported atomic runtime helpers in the
runtime or segment interner object.

These checks establish the listed regressions at this source checkpoint.
They do not add sanitizer or performance evidence, complete the x86-64
umbrella, or establish general ARM64 JIT/FFI admission. The broader limits
in [`README.md`](README.md) remain active. The preceding combined checkpoint
is retained in [rooted-values validation](rooted-values-validation-2026-10-25.md).

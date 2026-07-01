# Release and platform CI artifacts

2026-07-01:
- Rolling-release packaging now validates `b<major>.<minor>.<patch>` tags before
  artifact jobs and keeps Linux, macOS x86_64 target 13.0, and Windows UCRT
  archive creation behind `tools/release/build_artifact.sh`.
- Release archives are built from `make install DESTDIR` staging trees. The
  archive checks remain release-only, including macOS/Windows binary checks via
  the release harness and optional Darling/Wine runners.
- Normal GitHub CI now uses `tools/ci/platform_build.sh` for each platform and
  only builds plus runs a direct platform smoke (`jit.os`, `jit.arch`, and the
  threading module). It does not run the release archive harness or stock
  semantics suites.
- CI and release jobs install/use stock `luajit` for the Lua test harness. The
  local host currently has `/usr/bin/luajit` from the Debian package.
- Local verification:
  - `bash -n tools/release/build_artifact.sh tools/ci/platform_build.sh`;
  - `tools/ci/platform_build.sh linux-x86_64`;
  - `tools/release/build_artifact.sh linux-x86_64 b0.0.0 /tmp/lj-lockless-release-smoke`;
  - `LUA=luajit tools/ci/lua_test.sh --list` shows all release binary/archive
    cases.

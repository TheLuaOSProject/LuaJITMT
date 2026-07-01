# Release and platform CI artifacts

2026-07-01:
- Rolling-release packaging now validates `b<major>.<minor>.<patch>` tags before
  artifact jobs and keeps Linux, macOS x86_64 target 13.0, and Windows UCRT
  archive creation behind `tools/release/build_artifact.sh`.
- Release helper scripts now reject stale two-component tags such as `b1.1`;
  rerolled beta releases should use the three-component name, e.g. `b1.0.1`.
- Release notes still accept the legacy `b1.0` tag as a historical baseline,
  so `b1.0.1` can diff from it without making `b1.0` or `b1.1` a valid new
  rolling-release tag shape.
- Release archives are built from `make install DESTDIR` staging trees. The
  archive checks remain release-only, including macOS/Windows binary checks via
  the release harness and optional Darling/Wine runners.
- The publish job verifies the exact three expected archives before uploading:
  `linux-x86_64.tar.xz`, `macos-x86_64.tar.xz`, and
  `windows-x86_64-ucrt.zip`, all named with the same release tag.
- Artifact verification now checks `SHA256SUMS` when present, requires every
  archive to be checksummed either there or by a per-artifact `.sha256`, and
  inspects each archive for the `make install DESTDIR` layout plus BUILDINFO
  tag/platform/layout fields before publishing.
- Normal GitHub CI now uses `tools/ci/platform_build.sh` for each platform to
  build, run the binary directly, and dispatch the matching platform binary
  harness check (`jit.os`, `jit.arch`, and the threading module). It does not
  run release archive extraction checks or stock semantics suites.
- The opt-in M0 cross-platform smoke defaults now match the release platforms:
  Windows x86_64 UCRT and macOS x86_64 with deployment target 13.0. It only
  runs cross-platform work when `LJ_M0_PLATFORM_ENABLE` or
  `LJ_M0_PLATFORM_REQUIRE` is set; rolling-release jobs use the release suite.
- CI and release jobs install/use stock `luajit` for the Lua test harness. The
  local host currently has `/usr/bin/luajit` from the Debian package.
- Local verification:
  - `bash -n tools/release/build_artifact.sh tools/ci/platform_build.sh`;
  - `bash -n tools/release/release_notes.sh tools/release/verify_artifacts.sh`;
  - `LUA=luajit tools/ci/platform_build.sh linux-x86_64`;
  - `tools/release/build_artifact.sh linux-x86_64 b0.0.0 /tmp/lj-lockless-release-smoke`;
  - `LUA=luajit tools/ci/lua_test.sh --list` shows all release binary/archive
    cases.

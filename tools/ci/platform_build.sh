#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
usage: tools/ci/platform_build.sh <platform>

platform:
  linux-x86_64
  macos-x86_64
  windows-x86_64-ucrt

Builds the requested platform target and runs its platform smoke check.
Release install-tree archive checks live in tools/release/build_artifact.sh.
USAGE
  exit 2
}

if [ "$#" -ne 1 ]; then usage; fi

platform=$1
script_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
root=${LJ_CI_ROOT:-$script_root}
root=$(CDPATH= cd -- "$root" && pwd)
jobs=${JOBS:-${MAKE_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}}

case "$platform" in
  linux-x86_64|macos-x86_64|windows-x86_64-ucrt) ;;
  *) usage ;;
esac

make_clean() {
  make -C "$root" clean
  rm -f "$root/src/.lj-test-build-signature"
}

build_make() {
  make -C "$root" -j"$jobs" "$@"
}

run_release_binary_check() {
  local check=$1
  shift
  env LJ_RELEASE_RUN_STOCK=0 "$@" "$root/tools/ci/lua_test.sh" "$check"
}

case "$platform" in
  linux-x86_64)
    make_clean
    build_make
    run_release_binary_check release_linux_binary \
      LJ_RELEASE_REQUIRE=linux \
      LJ_RELEASE_LINUX_BIN="$root/src/luajit"
    ;;
  macos-x86_64)
    export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-13.0}
    macos_make_args=(TARGET_SYS=Darwin)
    if [ -n "${LJ_CI_MACOS_CC:-${LJ_RELEASE_MACOS_CC:-}}" ]; then
      macos_make_args+=(CC="${LJ_CI_MACOS_CC:-${LJ_RELEASE_MACOS_CC:-}}")
    fi
    if [ -n "${LJ_CI_MACOS_HOST_CC:-${LJ_RELEASE_MACOS_HOST_CC:-${HOST_CC:-}}}" ]; then
      macos_make_args+=(HOST_CC="${LJ_CI_MACOS_HOST_CC:-${LJ_RELEASE_MACOS_HOST_CC:-${HOST_CC:-}}}")
    fi
    if [ -n "${LJ_CI_MACOS_CROSS:-${LJ_RELEASE_MACOS_CROSS:-}}" ]; then
      macos_make_args+=(CROSS="${LJ_CI_MACOS_CROSS:-${LJ_RELEASE_MACOS_CROSS:-}}")
    fi
    make_clean
    build_make "${macos_make_args[@]}"
    run_release_binary_check release_macos_binary \
      LJ_RELEASE_REQUIRE=macos \
      LJ_RELEASE_MACOS_BIN="$root/src/luajit"
    ;;
  windows-x86_64-ucrt)
    cross=${LJ_CI_WINDOWS_CROSS:-${LJ_RELEASE_WINDOWS_CROSS:-x86_64-w64-mingw32ucrt-}}
    cc=${LJ_CI_WINDOWS_CC:-${LJ_RELEASE_WINDOWS_CC:-gcc}}
    windows_make_args=(
      HOST_CC="${HOST_CC:-gcc}"
      CROSS="$cross"
      CC="$cc"
      TARGET_SYS=Windows
    )
    make_clean
    build_make "${windows_make_args[@]}"
    run_release_binary_check release_windows_binary \
      LJ_RELEASE_REQUIRE=windows \
      LJ_RELEASE_WINDOWS_BIN="$root/src/luajit.exe" \
      LJ_RELEASE_WINDOWS_RUNNER="${LJ_RELEASE_WINDOWS_RUNNER:-wine}"
    ;;
esac

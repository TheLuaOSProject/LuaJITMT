#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
usage: tools/ci/platform_build.sh <platform>

platform:
  linux-x86_64
  macos-x86_64
  windows-x86_64-ucrt

Builds the requested platform target, runs it directly, and executes the
matching platform binary smoke test. Release install-tree archive checks live
in tools/release/build_artifact.sh.
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

smoke_code='print(jit.os, jit.arch); local threading = require("threading"); assert(type(threading.spawn) == "function"); assert(type(threading.gcstats) == "function")'

assert_platform_output() {
  local label=$1
  local out=$2
  local osname=$3
  if ! printf '%s\n' "$out" |
      grep -Eq "(^|[[:space:]])${osname}[[:space:]]+x64($|[[:space:]])"; then
    printf '%s\n' "$out" >&2
    printf 'CI %s smoke did not report %s x64\n' "$label" "$osname" >&2
    exit 1
  fi
}

run_direct_smoke() {
  local label=$1
  local bin=$2
  local osname=$3
  local out
  if [ ! -x "$bin" ]; then
    printf 'CI %s smoke binary is not executable: %s\n' "$label" "$bin" >&2
    exit 1
  fi
  if ! out=$("$bin" -e "$smoke_code" 2>&1); then
    printf '%s\n' "$out" >&2
    exit 1
  fi
  assert_platform_output "$label" "$out" "$osname"
  printf 'CI %s binary smoke passed\n' "$label"
}

run_windows_smoke() {
  local bin=$1
  local runner=${LJ_CI_WINDOWS_RUNNER:-${LJ_RELEASE_WINDOWS_RUNNER:-wine}}
  local out
  if [ ! -f "$bin" ]; then
    printf 'CI Windows smoke binary is missing: %s\n' "$bin" >&2
    exit 1
  fi
  if ! command -v "$runner" >/dev/null 2>&1; then
    printf 'CI Windows smoke runner is not in PATH: %s\n' "$runner" >&2
    exit 1
  fi
  if ! out=$(WINEDEBUG=-all "$runner" "$bin" -e "$smoke_code" 2>&1); then
    printf '%s\n' "$out" >&2
    exit 1
  fi
  assert_platform_output "Windows" "$out" "Windows"
  printf 'CI Windows binary smoke passed\n'
}

run_platform_test() {
  local case_name=$1
  local require=$2
  local bin_var=$3
  local bin=$4
  shift 4

  env \
    LJ_RELEASE_PREFIX="${PREFIX:-/usr/local}" \
    LJ_RELEASE_REQUIRE="$require" \
    LJ_RELEASE_RUN_STOCK=0 \
    "$bin_var=$bin" \
    "$@" \
    "$root/tools/ci/lua_test.sh" "$case_name"
}

case "$platform" in
  linux-x86_64)
    make_clean
    build_make
    run_direct_smoke "Linux" "$root/src/luajit" "Linux"
    run_platform_test \
      release_linux_binary linux LJ_RELEASE_LINUX_BIN "$root/src/luajit"
    ;;
  macos-x86_64)
    export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-13.0}
    macos_make_args=(
      TARGET_SYS=Darwin
      TARGET_FLAGS="${LJ_CI_MACOS_TARGET_FLAGS:-${LJ_RELEASE_MACOS_TARGET_FLAGS:--arch x86_64}}"
    )
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
    run_direct_smoke "macOS" "$root/src/luajit" "OSX"
    run_platform_test \
      release_macos_binary macos LJ_RELEASE_MACOS_BIN "$root/src/luajit"
    ;;
  windows-x86_64-ucrt)
    cross=${LJ_CI_WINDOWS_CROSS:-${LJ_RELEASE_WINDOWS_CROSS:-x86_64-w64-mingw32ucrt-}}
    cc=${LJ_CI_WINDOWS_CC:-${LJ_RELEASE_WINDOWS_CC:-gcc}}
    runner=${LJ_CI_WINDOWS_RUNNER:-${LJ_RELEASE_WINDOWS_RUNNER:-wine}}
    windows_make_args=(
      HOST_CC="${HOST_CC:-gcc}"
      CROSS="$cross"
      CC="$cc"
      TARGET_SYS=Windows
    )
    make_clean
    build_make "${windows_make_args[@]}"
    run_windows_smoke "$root/src/luajit.exe"
    run_platform_test \
      release_windows_binary windows LJ_RELEASE_WINDOWS_BIN "$root/src/luajit.exe" \
      LJ_RELEASE_WINDOWS_RUNNER="$runner"
    ;;
esac

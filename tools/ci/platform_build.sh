#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
usage: tools/ci/platform_build.sh <platform>

platform:
  linux-x86_64
  macos-x86_64
  windows-x86_64-ucrt

Builds the requested platform target, runs it directly on the matching host,
and executes the matching platform binary smoke test. Release install-tree
archive checks and Wine/Darling validation live in tools/release/build_artifact.sh.
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

is_windows_host() {
  case "$(uname -s 2>/dev/null || echo unknown)" in
    MINGW*|MSYS*|CYGWIN*) return 0 ;;
    *) return 1 ;;
  esac
}

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

run_windows_direct_smoke() {
  local bin=$1
  local out
  if ! is_windows_host; then
    printf 'CI Windows smoke must run on a Windows host; use tools/release/build_artifact.sh for Wine release validation\n' >&2
    exit 1
  fi
  if [ ! -f "$bin" ]; then
    printf 'CI Windows smoke binary is missing: %s\n' "$bin" >&2
    exit 1
  fi
  if ! out=$("$bin" -e "$smoke_code" 2>&1); then
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
    if ! is_windows_host; then
      printf 'CI Windows smoke must run on a Windows host; use tools/release/build_artifact.sh for Wine release validation\n' >&2
      exit 1
    fi
    cross=${LJ_CI_WINDOWS_CROSS-}
    cc=${LJ_CI_WINDOWS_CC:-${LJ_RELEASE_WINDOWS_CC:-gcc}}
    windows_make_args=(
      HOST_CC="${HOST_CC:-gcc}"
      CROSS="$cross"
      CC="$cc"
      TARGET_SYS=Windows
    )
    make_clean
    build_make "${windows_make_args[@]}"
    run_windows_direct_smoke "$root/src/luajit.exe"
    ;;
esac

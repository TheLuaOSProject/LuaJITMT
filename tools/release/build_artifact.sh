#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
usage: tools/release/build_artifact.sh <platform> <tag> [out-dir]

platform:
  linux-x86_64
  macos-x86_64
  windows-x86_64-ucrt

The script builds a clean release artifact, stages an install-style tree,
runs release smoke/archive checks unless LJ_RELEASE_RUN_TESTS=0, and writes
the archive plus per-artifact checksum into out-dir. Set
LJ_RELEASE_RUN_STOCK=1 to include the full stock LuaJIT test suite.
USAGE
  exit 2
}

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then usage; fi

platform=$1
tag=$2
out_dir=${3:-release-artifacts/${tag}}
script_root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
root=${LJ_RELEASE_ROOT:-$script_root}
root=$(CDPATH= cd -- "$root" && pwd)
test_root=${LJ_RELEASE_TEST_ROOT:-$script_root}
test_root=$(CDPATH= cd -- "$test_root" && pwd)
jobs=${JOBS:-${MAKE_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}}
prefix=${PREFIX:-/usr/local}
commit=$(git -C "$root" rev-parse HEAD)
stage_parent=${LJ_RELEASE_STAGE_PARENT:-$(mktemp -d)}
keep_stage=${LJ_RELEASE_KEEP_STAGE:-0}
run_tests=${LJ_RELEASE_RUN_TESTS:-1}

case "$platform" in
  linux-x86_64|macos-x86_64|windows-x86_64-ucrt) ;;
  *) usage ;;
esac

pkg="LuaJITMT-${tag}-${platform}"
stage="${stage_parent}/${pkg}"

cleanup() {
  if [ "$keep_stage" != "1" ]; then
    rm -rf "$stage_parent"
  else
    echo "kept release staging tree: $stage"
  fi
}
trap cleanup EXIT

mkdir -p "$out_dir"
rm -rf "$stage"

make_clean() {
  make -C "$root" clean
  rm -f "$root/src/.lj-test-build-signature"
}

build_make() {
  make -C "$root" -j"$jobs" "$@"
}

install_doc() {
  local docdir=$1
  local layout=$2
  mkdir -p "$docdir"
  install -m 0644 "$root/README" "$docdir/README"
  install -m 0644 "$root/COPYRIGHT" "$docdir/COPYRIGHT"
  {
    printf 'LuaJITMT %s\n' "$tag"
    printf 'commit: %s\n' "$commit"
    printf 'platform: %s\n' "$platform"
    printf 'arch: x86_64\n'
    printf 'relver: %s\n' "$(cat "$root/src/luajit_relver.txt")"
    printf 'layout: %s\n' "$layout"
  } > "$docdir/BUILDINFO"
}

archive_stage() {
  local archive
  if command -v xz >/dev/null 2>&1; then
    archive="${out_dir}/${pkg}.tar.xz"
    (cd "$stage_parent" && tar -cJf "$archive" "$pkg")
  else
    archive="${out_dir}/${pkg}.tar.gz"
    (cd "$stage_parent" && tar -czf "$archive" "$pkg")
  fi
  (cd "$out_dir" && checksum "$(basename "$archive")" > "$(basename "$archive").sha256")
  echo "$archive"
}

archive_zip_stage() {
  local archive="${out_dir}/${pkg}.zip"
  (cd "$stage_parent" && zip -qr "$archive" "$pkg")
  (cd "$out_dir" && checksum "$(basename "$archive")" > "$(basename "$archive").sha256")
  echo "$archive"
}

checksum() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$@"
  else
    shasum -a 256 "$@"
  fi
}

run_release_test() {
  if [ "$run_tests" = "0" ]; then return; fi
  case "$platform" in
    linux-x86_64)
      LJ_RELEASE_REQUIRE=linux \
      LJ_RELEASE_LINUX_BIN="${stage}${prefix}/bin/luajit" \
        "$test_root/tools/ci/lua_test.sh" release_linux_binary
      ;;
    macos-x86_64)
      LJ_RELEASE_REQUIRE=macos \
      LJ_RELEASE_MACOS_BIN="${stage}${prefix}/bin/luajit" \
        "$test_root/tools/ci/lua_test.sh" release_macos_binary
      ;;
    windows-x86_64-ucrt)
      LJ_RELEASE_REQUIRE=windows \
      LJ_RELEASE_WINDOWS_BIN="${stage}${prefix}/bin/luajit.exe" \
      LJ_RELEASE_WINDOWS_RUNNER="${LJ_RELEASE_WINDOWS_RUNNER:-wine}" \
        "$test_root/tools/ci/lua_test.sh" release_windows_binary
      ;;
  esac
}

run_release_archive_test() {
  if [ "$run_tests" = "0" ]; then return; fi
  local archive=$1
  case "$platform" in
    linux-x86_64)
      LJ_RELEASE_REQUIRE=linux \
      LJ_RELEASE_LINUX_ARCHIVE="$archive" \
        "$test_root/tools/ci/lua_test.sh" release_linux_archive
      ;;
    macos-x86_64)
      LJ_RELEASE_REQUIRE=macos \
      LJ_RELEASE_MACOS_ARCHIVE="$archive" \
      LJ_RELEASE_MACOS_RUNNER="${LJ_RELEASE_MACOS_RUNNER:-}" \
        "$test_root/tools/ci/lua_test.sh" release_macos_archive
      ;;
    windows-x86_64-ucrt)
      LJ_RELEASE_REQUIRE=windows \
      LJ_RELEASE_WINDOWS_ARCHIVE="$archive" \
      LJ_RELEASE_WINDOWS_RUNNER="${LJ_RELEASE_WINDOWS_RUNNER:-wine}" \
        "$test_root/tools/ci/lua_test.sh" release_windows_archive
      ;;
  esac
}

case "$platform" in
  linux-x86_64)
    make_clean
    build_make
    make -C "$root" install DESTDIR="$stage" PREFIX="$prefix"
    install_doc "${stage}${prefix}/share/doc/luajitmt" "make install DESTDIR prefix tree"
    run_release_test
    archive=$(archive_stage)
    run_release_archive_test "$archive"
    ;;
  macos-x86_64)
    export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-13.0}
    macos_make_args=(TARGET_SYS=Darwin)
    if [ -n "${LJ_RELEASE_MACOS_CC:-}" ]; then
      macos_make_args+=(CC="$LJ_RELEASE_MACOS_CC")
    fi
    if [ -n "${LJ_RELEASE_MACOS_HOST_CC:-${HOST_CC:-}}" ]; then
      macos_make_args+=(HOST_CC="${LJ_RELEASE_MACOS_HOST_CC:-${HOST_CC:-}}")
    fi
    if [ -n "${LJ_RELEASE_MACOS_CROSS:-}" ]; then
      macos_make_args+=(CROSS="$LJ_RELEASE_MACOS_CROSS")
    fi
    make_clean
    build_make "${macos_make_args[@]}"
    make -C "$root" install DESTDIR="$stage" PREFIX="$prefix" \
      "${macos_make_args[@]}"
    install_doc "${stage}${prefix}/share/doc/luajitmt" "make install DESTDIR prefix tree"
    run_release_test
    archive=$(archive_stage)
    run_release_archive_test "$archive"
    ;;
  windows-x86_64-ucrt)
    cross=${LJ_RELEASE_WINDOWS_CROSS:-x86_64-w64-mingw32ucrt-}
    cc=${LJ_RELEASE_WINDOWS_CC:-gcc}
    make_clean
    build_make HOST_CC="${HOST_CC:-gcc}" CROSS="$cross" CC="$cc" TARGET_SYS=Windows
    make -C "$root" install DESTDIR="$stage" PREFIX="$prefix" \
      HOST_CC="${HOST_CC:-gcc}" CROSS="$cross" CC="$cc" TARGET_SYS=Windows
    install_doc "${stage}${prefix}/share/doc/luajitmt" "make install DESTDIR prefix tree"
    {
      printf 'toolchain: %s%s\n' "$cross" "$cc"
      printf 'runtime: UCRT\n'
    } >> "${stage}${prefix}/share/doc/luajitmt/BUILDINFO"
    run_release_test
    archive=$(archive_zip_stage)
    run_release_archive_test "$archive"
    ;;
esac

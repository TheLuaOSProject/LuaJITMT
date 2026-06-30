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
runs the release binary test unless LJ_RELEASE_RUN_TESTS=0, and writes the
archive plus per-artifact checksum into out-dir.
USAGE
  exit 2
}

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then usage; fi

platform=$1
tag=$2
out_dir=${3:-release-artifacts/${tag}}
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
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
        "$root/tools/ci/lua_test.sh" release_linux_binary
      ;;
    macos-x86_64)
      LJ_RELEASE_REQUIRE=macos \
      LJ_RELEASE_MACOS_BIN="${stage}${prefix}/bin/luajit" \
        "$root/tools/ci/lua_test.sh" release_macos_binary
      ;;
    windows-x86_64-ucrt)
      LJ_RELEASE_REQUIRE=windows \
      LJ_RELEASE_WINDOWS_BIN="${stage}/bin/luajit.exe" \
      LJ_RELEASE_WINDOWS_RUNNER="${LJ_RELEASE_WINDOWS_RUNNER:-wine}" \
        "$root/tools/ci/lua_test.sh" release_windows_binary
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
    archive_stage
    ;;
  macos-x86_64)
    export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-13.0}
    make_clean
    build_make TARGET_SYS=Darwin
    make -C "$root" install DESTDIR="$stage" PREFIX="$prefix" TARGET_SYS=Darwin
    install_doc "${stage}${prefix}/share/doc/luajitmt" "make install DESTDIR prefix tree"
    run_release_test
    archive_stage
    ;;
  windows-x86_64-ucrt)
    cross=${LJ_RELEASE_WINDOWS_CROSS:-x86_64-w64-mingw32ucrt-}
    cc=${LJ_RELEASE_WINDOWS_CC:-gcc}
    make_clean
    build_make HOST_CC="${HOST_CC:-gcc}" CROSS="$cross" CC="$cc" TARGET_SYS=Windows
    mkdir -p "$stage/bin" "$stage/lib" "$stage/include/luajit-2.1" \
      "$stage/share/luajit-2.1/jit" "$stage/share/doc/luajitmt"
    install -m 0755 "$root/src/luajit.exe" "$stage/bin/luajit.exe"
    install -m 0755 "$root/src/lua51.dll" "$stage/bin/lua51.dll"
    install -m 0644 "$root/src/libluajit-5.1.dll.a" "$stage/lib/libluajit-5.1.dll.a"
    for f in lua.h lualib.h lauxlib.h luaconf.h lua.hpp luajit.h; do
      install -m 0644 "$root/src/$f" "$stage/include/luajit-2.1/$f"
    done
    for f in bc.lua bcsave.lua dump.lua p.lua v.lua zone.lua \
      dis_x86.lua dis_x64.lua dis_arm.lua dis_arm64.lua dis_arm64be.lua \
      dis_ppc.lua dis_mips.lua dis_mipsel.lua dis_mips64.lua \
      dis_mips64el.lua dis_mips64r6.lua dis_mips64r6el.lua vmdef.lua; do
      install -m 0644 "$root/src/jit/$f" "$stage/share/luajit-2.1/jit/$f"
    done
    install_doc "$stage/share/doc/luajitmt" "Windows install-style prefix tree"
    {
      printf 'toolchain: %s%s\n' "$cross" "$cc"
      printf 'runtime: UCRT\n'
    } >> "$stage/share/doc/luajitmt/BUILDINFO"
    run_release_test
    archive_zip_stage
    ;;
esac

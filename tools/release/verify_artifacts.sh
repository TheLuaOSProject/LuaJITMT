#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
usage: tools/release/verify_artifacts.sh <tag> [dist-dir]

Verifies that a rolling release dist directory contains exactly the expected
x86_64 Linux, macOS, and Windows archives for a b<major>.<minor>.<patch> tag.
It also checks checksums and the make-install archive layout.
USAGE
  exit 2
}

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then usage; fi

tag=$1
dist=${2:-dist}

if [[ ! "$tag" =~ ^b[0-9]+[.][0-9]+[.][0-9]+$ ]]; then
  printf 'release tag must be b<major>.<minor>.<patch>; got %s\n' "$tag" >&2
  exit 2
fi

if [ ! -d "$dist" ]; then
  printf 'release artifact directory does not exist: %s\n' "$dist" >&2
  exit 1
fi
dist=$(CDPATH= cd -- "$dist" && pwd)
prefix=${LJ_RELEASE_PREFIX:-${PREFIX:-/usr/local}}
prefix=${prefix%/}
if [ "$prefix" = "/" ]; then prefix=""; fi
if [ -n "$prefix" ] && [ "${prefix#/}" = "$prefix" ]; then
  prefix="/$prefix"
fi

expected=(
  "LuaJITMT-${tag}-linux-x86_64.tar.xz"
  "LuaJITMT-${tag}-macos-x86_64.tar.xz"
  "LuaJITMT-${tag}-windows-x86_64-ucrt.zip"
)

is_expected_file() {
  local base=$1
  local name
  [ "$base" = "SHA256SUMS" ] && return 0
  for name in "${expected[@]}"; do
    [ "$base" = "$name" ] && return 0
    [ "$base" = "$name.sha256" ] && return 0
  done
  return 1
}

check_sha256_file() {
  local sums=$1
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum -c "$sums"
  else
    shasum -a 256 -c "$sums"
  fi
}

checksum_lists() {
  local name=$1
  awk '
    {
      name = $NF
      sub(/^\*/, "", name)
      sub(/^\.\//, "", name)
      if (name == want) found = 1
    }
    END { exit(found ? 0 : 1) }
  ' want="$name" "$dist/SHA256SUMS"
}

archive_list() {
  local archive=$1
  case "$archive" in
    *.zip)
      if ! command -v unzip >/dev/null 2>&1; then
        printf 'unzip is required to verify %s\n' "$(basename -- "$archive")" >&2
        return 1
      fi
      unzip -Z1 "$archive"
      ;;
    *.tar.xz)
      tar -tf "$archive"
      ;;
    *)
      printf 'unsupported release archive type: %s\n' "$(basename -- "$archive")" >&2
      return 1
      ;;
  esac
}

archive_file() {
  local archive=$1
  local path=$2
  case "$archive" in
    *.zip) unzip -p "$archive" "$path" ;;
    *.tar.xz) tar -xOf "$archive" "$path" ;;
  esac
}

buildinfo_has_platform() {
  local buildinfo=$1
  local platform=$2
  if printf '%s\n' "$buildinfo" | grep -Fxq "platform: $platform"; then
    return 0
  fi
  case "$platform" in
    linux-x86_64)
      printf '%s\n' "$buildinfo" | grep -Fxq 'platform: linux' &&
        printf '%s\n' "$buildinfo" | grep -Fxq 'arch: x86_64'
      ;;
    macos-x86_64)
      printf '%s\n' "$buildinfo" | grep -Fxq 'platform: macos' &&
        printf '%s\n' "$buildinfo" | grep -Fxq 'arch: x86_64'
      ;;
    windows-x86_64-ucrt)
      printf '%s\n' "$buildinfo" | grep -Fxq 'platform: windows' &&
        printf '%s\n' "$buildinfo" | grep -Fxq 'arch: x86_64'
      ;;
    *)
      return 1
      ;;
  esac
}

require_archive_path() {
  local archive_name=$1
  local list=$2
  local path=$3
  if ! grep -Fxq "$path" "$list"; then
    printf 'archive %s missing install-layout path: %s\n' \
      "$archive_name" "$path" >&2
    failed=1
  fi
}

verify_archive_layout() {
  local name=$1
  local platform=$2
  local bin=$3
  shift 3
  local archive="$dist/$name"
  local root="LuaJITMT-${tag}-${platform}"
  local list
  local buildinfo

  list=$(mktemp)
  if ! archive_list "$archive" > "$list"; then
    rm -f "$list"
    failed=1
    return
  fi

  require_archive_path "$name" "$list" "$root/"
  require_archive_path "$name" "$list" "$root${prefix}/bin/$bin"
  require_archive_path "$name" "$list" "$root${prefix}/include/luajit-2.1/lua.h"
  require_archive_path "$name" "$list" "$root${prefix}/include/luajit-2.1/lualib.h"
  require_archive_path "$name" "$list" "$root${prefix}/include/luajit-2.1/lauxlib.h"
  require_archive_path "$name" "$list" "$root${prefix}/include/luajit-2.1/luaconf.h"
  require_archive_path "$name" "$list" "$root${prefix}/include/luajit-2.1/lua.hpp"
  require_archive_path "$name" "$list" "$root${prefix}/include/luajit-2.1/luajit.h"
  require_archive_path "$name" "$list" "$root${prefix}/lib/pkgconfig/luajit.pc"
  require_archive_path "$name" "$list" "$root${prefix}/share/luajit-2.1/jit/vmdef.lua"
  require_archive_path "$name" "$list" "$root${prefix}/share/doc/luajitmt/README"
  require_archive_path "$name" "$list" "$root${prefix}/share/doc/luajitmt/COPYRIGHT"
  require_archive_path "$name" "$list" "$root${prefix}/share/doc/luajitmt/BUILDINFO"
  while [ "$#" -gt 0 ]; do
    require_archive_path "$name" "$list" "$root${prefix}/$1"
    shift
  done
  rm -f "$list"

  if ! buildinfo=$(archive_file "$archive" \
      "$root${prefix}/share/doc/luajitmt/BUILDINFO"); then
    printf 'archive %s missing readable BUILDINFO\n' "$name" >&2
    failed=1
    return
  fi
  if ! printf '%s\n' "$buildinfo" | grep -Fxq "LuaJITMT $tag"; then
    printf 'archive %s BUILDINFO does not name %s\n' "$name" "$tag" >&2
    failed=1
  fi
  if ! buildinfo_has_platform "$buildinfo" "$platform"; then
    printf 'archive %s BUILDINFO does not name platform %s\n' \
      "$name" "$platform" >&2
    failed=1
  fi
  if ! printf '%s\n' "$buildinfo" |
      grep -Eq '^layout: make install DESTDIR([[:space:]]|$)'; then
    printf 'archive %s BUILDINFO does not confirm make-install layout\n' \
      "$name" >&2
    failed=1
  fi
}

failed=0
for name in "${expected[@]}"; do
  if [ ! -s "$dist/$name" ]; then
    printf 'missing required release artifact: %s\n' "$name" >&2
    failed=1
  fi
done

while IFS= read -r -d '' path; do
  base=$(basename -- "$path")
  if ! is_expected_file "$base"; then
    printf 'unexpected release artifact file: %s\n' "$base" >&2
    failed=1
  fi
done < <(find "$dist" -maxdepth 1 -type f -print0)

[ "$failed" -eq 0 ] || exit 1

if [ -f "$dist/SHA256SUMS" ]; then
  (cd "$dist" && check_sha256_file SHA256SUMS)
fi

for name in "${expected[@]}"; do
  if [ -f "$dist/$name.sha256" ]; then
    (cd "$dist" && check_sha256_file "$name.sha256")
  elif [ ! -f "$dist/SHA256SUMS" ]; then
    printf 'missing checksum for release artifact: %s\n' "$name" >&2
    failed=1
  fi
  if [ -f "$dist/SHA256SUMS" ] && ! checksum_lists "$name"; then
    printf 'SHA256SUMS does not list release artifact: %s\n' "$name" >&2
    failed=1
  fi
done

verify_archive_layout \
  "LuaJITMT-${tag}-linux-x86_64.tar.xz" \
  linux-x86_64 luajit \
  lib/libluajit-5.1.a \
  lib/libluajit-5.1.so
verify_archive_layout \
  "LuaJITMT-${tag}-macos-x86_64.tar.xz" \
  macos-x86_64 luajit \
  lib/libluajit-5.1.a \
  lib/libluajit-5.1.dylib
verify_archive_layout \
  "LuaJITMT-${tag}-windows-x86_64-ucrt.zip" \
  windows-x86_64-ucrt luajit.exe \
  bin/lua51.dll \
  lib/libluajit-5.1.dll.a

[ "$failed" -eq 0 ] || exit 1

printf 'verified release artifacts for %s:\n' "$tag"
printf '  %s\n' "${expected[@]}"

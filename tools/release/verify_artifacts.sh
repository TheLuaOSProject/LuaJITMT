#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat >&2 <<'USAGE'
usage: tools/release/verify_artifacts.sh <tag> [dist-dir]

Verifies that a rolling release dist directory contains exactly the expected
x86_64 Linux, macOS, and Windows archives for a b<major>.<minor>.<patch> tag.
Per-archive .sha256 files and SHA256SUMS are allowed when present.
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
done < <(find "$dist" -maxdepth 1 -type f \( -name 'LuaJITMT-*' -o -name '*.sha256' -o -name 'SHA256SUMS' \) -print0)

[ "$failed" -eq 0 ] || exit 1

for name in "${expected[@]}"; do
  if [ -f "$dist/$name.sha256" ]; then
    (cd "$dist" && check_sha256_file "$name.sha256")
  fi
done

printf 'verified release artifacts for %s:\n' "$tag"
printf '  %s\n' "${expected[@]}"

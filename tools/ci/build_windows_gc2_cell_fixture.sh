#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 4 ]; then
  printf 'usage: %s <target-cc> <build-root> <test-root> <output.exe>\n' \
    "$0" >&2
  exit 2
fi

cc=$1
build_root=$2
test_root=$3
output=$4
objects=()

if ! command -v "$cc" >/dev/null 2>&1; then
  printf '%s is required to build the Windows GC2 thread-cell fixture\n' \
    "$cc" >&2
  exit 1
fi
if [ ! -f "$test_root/tests/t-windows-gc2-cell.c" ]; then
  printf 'Windows GC2 thread-cell fixture source is missing\n' >&2
  exit 1
fi

for object in "$build_root"/src/*.o; do
  case "${object##*/}" in
    luajit.o) ;;
    *) objects+=("$object") ;;
  esac
done
if [ "${#objects[@]}" -eq 0 ]; then
  printf 'Windows GC2 thread-cell fixture found no production objects\n' >&2
  exit 1
fi

"$cc" -std=gnu11 -O2 -Wall -Wextra -Werror -mcx16 -static \
  -static-libgcc \
  -I"$build_root/src" "$test_root/tests/t-windows-gc2-cell.c" \
  "${objects[@]}" -lm -lsynchronization -o "$output"

printf 'Windows GC2 production thread-cell fixture built: %s\n' "$output"
